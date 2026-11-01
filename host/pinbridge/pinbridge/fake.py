"""Small independent in-memory serial fake for protocol-level host tests."""

import re
import threading
import time

from .pins_gen import CFG_HASH, PINS


class FakeSerial:
    def __init__(self, *a, **k):
        self.q = []
        self.closed = False
        self.lock = threading.Lock()
        self.up = 0
        self.modes = {p[0]: "off" for p in PINS}
        self.values = {p[0]: 0 for p in PINS}
        self.values["PB1"] = 1  # released power button; PB3 low means paper present
        self.adc_values = {
            "PB1": (412, 1219),
            "PB3": (80, 240),
            "PC4": (512, 1650),
            "PC5": (620, 2000),
        }
        self.timeout = 0.1

    def write(self, data):
        for line in data.decode().splitlines():
            self._cmd(line)
        return len(data)

    def _cmd(self, s):
        t = s.split()
        c = t[0].upper()
        out = "ok"
        if c == "HEL":
            self.up += 10
            out = f"ok jlpin 2 {CFG_HASH} up={self.up}"
        elif c == "MOD" and len(t) == 3:
            pin = t[1].upper()
            mode = t[2].lower()
            p = next(
                (p for p in PINS if p[0] == pin or (p[6] and p[6].upper() == pin)), None
            )
            if not p:
                out = f"err pin {t[1]}"
            elif mode not in ("hiz", "in", "up", "dn", "lo", "hi", "adc", "pwm"):
                out = f"err arg {t[2]}"
            elif (
                mode != "hiz"
                and {
                    "in": "in",
                    "up": "pu",
                    "dn": "pd",
                    "lo": "out",
                    "hi": "out",
                    "adc": "adc",
                    "pwm": "pwm",
                }.get(mode)
                not in p[3]
            ):
                out = f"err cap {t[1]}"
            else:
                self.modes[p[0]] = mode
                out = "ok"
        elif c == "SET":
            staged = []
            for item in t[1:]:
                m = re.fullmatch(r"([^=]+)=([01])", item)
                if not m:
                    out = f"err arg {item}"
                    break
                pin = m[1].upper()
                p = next((p for p in PINS if p[0] == pin), None)
                if not p:
                    out = f"err pin {pin}"
                    break
                if self.modes[pin] not in ("lo", "hi"):
                    out = f"err mod {item}"
                    break
                staged.append((pin, int(m[2])))
            else:
                for pin, v in staged:
                    self.values[pin] = v
                    self.modes[pin] = "hi" if v else "lo"
        elif c == "RST":
            self.modes = {p[0]: "off" for p in PINS}
            self.values = {p[0]: 0 for p in PINS}
        elif c in ("GET", "STA"):
            names = t[1:] or [p[0] for p in PINS]
            out = "ok"
            for pin in names:
                p = next((p for p in PINS if p[0] == pin.upper()), None)
                if not p:
                    out = f"err pin {pin}"
                    break
                m = self.modes[p[0]]
                if c == "GET" and m not in ("in", "up", "dn", "lo", "hi"):
                    out = f"err mod {pin}"
                    break
                if c == "STA" and m == "off":
                    continue
                out += f" {p[0]}={self.values[p[0]] if c == 'GET' else m}"
        elif c == "WDG":
            if (
                len(t) != 2
                or not t[1].isdigit()
                or (int(t[1]) and not 100 <= int(t[1]) <= 3600000)
            ):
                out = f"err arg {t[-1]}"
        elif c == "DBG":
            if len(t) != 2 or t[1].lower() not in ("on", "off"):
                out = "err arg"
        elif c == "LSN":
            if len(t) < 2:
                out = "err arg"
            elif t[1].lower() == "off":
                pass
            else:
                for pin in t[1:]:
                    p = next((p for p in PINS if p[0] == pin.upper()), None)
                    if not p:
                        out = f"err pin {pin}"
                        break
                    if self.modes[p[0]] not in ("in", "up", "dn"):
                        out = f"err mod {pin}"
                        break
        elif c == "ADC":
            if len(t) < 2:
                out = "err arg"
            else:
                readings = []
                for name in t[1:]:
                    pin = name.upper()
                    p = next((p for p in PINS if p[0] == pin), None)
                    if not p:
                        out = f"err pin {pin}"
                        break
                    if self.modes[pin] != "adc":
                        out = f"err mod {pin}"
                        break
                    raw, mv = self.adc_values.get(pin, (0, 0))
                    readings.append(f"{pin}={raw}/{mv}")
                else:
                    out = "ok " + " ".join(readings)
        else:
            out = f"err cmd {t[0]}"
        with self.lock:
            self.q.append((out + "\n").encode())

    def readline(self):
        deadline = time.monotonic() + 0.1
        while time.monotonic() < deadline:
            with self.lock:
                if self.q:
                    return self.q.pop(0)
            time.sleep(0.001)
        return b""

    def close(self):
        self.closed = True
