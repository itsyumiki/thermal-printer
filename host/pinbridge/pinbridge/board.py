"""Synchronous command API over a pyserial-compatible object."""

from __future__ import annotations

import threading
import time

from .pins_gen import CFG_HASH, PINS
from .protocol import *


class Board:
    def __init__(self, port=None, *, timeout=0.5, serial_obj=None):
        self.port, self.timeout, self.serial = port, timeout, serial_obj
        self._lock = threading.Lock()
        self._events = []
        self._reader = None
        self._stop = threading.Event()
        self._reset = False
        self.on_event = None
        self.up = None

    def open(self):
        if self.serial is None:
            try:
                import serial
            except ImportError as e:
                raise RuntimeError("Install pinbridge with pip install .") from e
            if self.port is None:
                raise ValueError(
                    "pass --port; USB VID/PID auto-detection is not configured"
                )
            self.serial = serial.Serial(
                self.port, baudrate=115200, timeout=0.1, write_timeout=self.timeout
            )
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        data = self.command("HEL")
        if len(data) < 4 or data[2] != CFG_HASH:
            raise ProtocolError(
                f"configuration hash mismatch: device={data[2] if len(data) > 2 else '?'} host={CFG_HASH}"
            )
        if data[0] != "jlpin" or data[1] != "2":
            raise ProtocolError(f"unsupported protocol version {data[0]}")
        self.up = int(data[3].split("=", 1)[1])
        return self

    def _read_loop(self):
        while not self._stop.is_set():
            try:
                raw = self.serial.readline()
            except Exception:
                return
            if not raw:
                continue
            line = raw.decode("ascii", "replace").rstrip("\r\n")
            if line.startswith(("!", "#")):
                if line.startswith("!") and "wdg" not in line:
                    event = ("pin", line)
                elif line == "!wdg":
                    event = ("watchdog", None)
                else:
                    event = ("debug", line)
                if self.on_event:
                    self.on_event(event)
                else:
                    self._events.append(event)
            else:
                self._events.append(("reply", line))

    def command(self, line):
        if self._reset:
            raise BoardReset("device reboot detected; call ack_reset()")
        with self._lock:
            self.serial.write((line.strip() + "\n").encode("ascii"))
            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline:
                for i, (kind, val) in enumerate(self._events):
                    if kind == "reply":
                        del self._events[i]
                        parts = parse_reply(val)
                        if line.strip().upper() == "HEL" and len(parts) >= 3:
                            newup = int(parts[3].split("=", 1)[1])
                            if self.up is not None and newup < self.up:
                                self._reset = True
                                if self.on_event:
                                    self.on_event(("reset", newup))
                                raise BoardReset("device reboot detected")
                            self.up = newup
                        return parts
                time.sleep(0.005)
            raise TimeoutError(f"timeout waiting for reply to {line!r}")

    def mod(self, pin, mode):
        return self.command(f"MOD {self._pin(pin)} {mode}")

    def set(self, **pins):
        return self.command(
            "SET " + " ".join(f"{self._pin(k)}={int(v)}" for k, v in pins.items())
        )

    def get(self, *pins):
        return self.command(
            "GET" + (" " + " ".join(map(self._pin, pins)) if pins else "")
        )

    def adc(self, *pins):
        return self.command("ADC " + " ".join(pins))

    def listen(self, *pins):
        return self.command("LSN " + " ".join(map(self._pin, pins)))

    def sta(self, *pins):
        return self.command(
            "STA" + (" " + " ".join(map(self._pin, pins)) if pins else "")
        )

    def wdg(self, ms):
        return self.command(f"WDG {int(ms)}")

    def rst(self):
        return self.command("RST")

    def ack_reset(self):
        self._reset = False
        self.up = None

    @staticmethod
    def _pin(pin):
        s = str(pin).upper()
        for p in PINS:
            if s == p[0] or (p[6] and s == p[6].upper()):
                return p[0]
        return s

    def close(self):
        try:
            if self.serial:
                self.command("RST")
        except Exception:
            pass
        self._stop.set()
        if self.serial:
            try:
                self.serial.close()
            except Exception:
                pass

    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        self.close()
