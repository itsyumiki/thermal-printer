"""Rewritten task adapter for pinbridge; no old gpiodemo wire protocol is used.

Provenance: same-owner AGPLv3 gpiodemo/master GUI task-queue concept (commit
1224fb6^); this adapter is a new implementation for this GPLv3-or-later repo.
"""

from __future__ import annotations

import queue
import threading
import time

from ..board import Board
from ..fake import FakeSerial
from ..pins_gen import PINS
from ..protocol import BoardReset

DANGEROUS_PINS = frozenset(("PA0", "PA1", "PB8", "PB9", "PB10", "PB11", "PA9", "PA10", "PC2", "PC3"))


def modes_for_pin(pin_name: str) -> tuple[str, ...]:
    """Only show modes supported by that pin's generated capabilities."""
    p = next(p for p in PINS if p[0] == pin_name.upper())
    caps = set(p[3])
    modes = ["hiz"]
    if "in" in caps:
        modes.append("in")
    if "pu" in caps:
        modes.append("up")
    if "pd" in caps:
        modes.append("dn")
    if "out" in caps:
        modes.extend(("lo", "hi"))
    return tuple(modes)


def output_confirmation_required(pin: str, mode: str, confirmed: set[str]) -> bool:
    """Pure gate predicate. PB11 requires its name confirmation at UI layer."""
    return (
        pin.upper() in DANGEROUS_PINS
        and mode.lower() in ("lo", "hi")
        and pin.upper() not in confirmed
    )


class TaskAdapter:
    """Consumes legacy-shaped task dicts and invokes their callback on worker thread."""

    def __init__(
        self,
        task_queue: queue.Queue,
        *,
        port=None,
        mock=False,
        board=None,
        watchdog_ms=5000,
        clock=time.monotonic,
        start=True,
    ):
        self.tasks = task_queue
        self.board = board or Board(port, serial_obj=FakeSerial() if mock else None)
        self.watchdog_ms = watchdog_ms
        self.clock = clock
        self.stop_event = threading.Event()
        self.events = queue.Queue()
        self._thread = None
        self._keepalive = None
        self._last_hello = 0.0
        if start:
            self.start()

    def start(self):
        self.board.on_event = self._on_event
        self.board.open()
        self._last_hello = self.clock()
        self._thread = threading.Thread(
            target=self._run, name="pinbridge-gui-adapter", daemon=True
        )
        self._thread.start()
        self._keepalive = threading.Thread(
            target=self._keepalive_loop, name="pinbridge-gui-keepalive", daemon=True
        )
        self._keepalive.start()
        return self

    def _on_event(self, event):
        self.events.put(event)

    def _keepalive_loop(self):
        interval = max(0.1, self.watchdog_ms / 3000.0)
        while not self.stop_event.wait(interval):
            try:
                self.board.command("HEL")
                self._last_hello = self.clock()
            except BoardReset as exc:
                self.events.put(("reset", str(exc)))
            except Exception as exc:
                self.events.put(("error", str(exc)))

    def _run(self):
        while not self.stop_event.is_set():
            try:
                task = self.tasks.get(timeout=0.05)
            except queue.Empty:
                continue
            if not task.get("window", True):
                continue
            action, pin, value, cb = (
                task.get("action"),
                task.get("pin"),
                task.get("value"),
                task.get("callback"),
            )
            result = {"type": "ack"}
            try:
                if action == "handshake":
                    result = {"type": "handshake_ack", "info": ["jlpin", "2"]}
                elif action == "get_status":
                    result = {"type": "status", "info": self.board.sta()[0:]}
                elif action == "set_direction":
                    mode = "in" if str(value).upper() == "IN" else "lo"
                    self.board.mod(pin, mode)
                elif action == "set_pullup":
                    self.board.mod(pin, "up" if str(value).upper() == "ON" else "in")
                elif action == "get_value":
                    parts = self.board.get(pin)
                    result = {
                        "type": "data",
                        "pin": pin,
                        "value": parts[0].split("=", 1)[1],
                    }
                elif action == "set_value":
                    self.board.set(**{pin: str(value).upper() in ("ON", "HIGH", "1")})
                elif action == "set_listen":
                    self.board.command(
                        ("LSN " if str(value).upper() == "ON" else "LSN off ") + pin
                    )
                elif action == "goodbye":
                    self.close()
                    result = {"type": "goodbye_ack"}
                else:
                    result = {"type": "error", "info": [f"unknown action {action}"]}
            except BoardReset as exc:
                self.events.put(("reset", str(exc)))
                result = {"type": "error", "info": [str(exc)]}
            except Exception as exc:
                result = {"type": "error", "info": [str(exc)]}
            if cb:
                cb(result)

    def close(self):
        if self.stop_event.is_set():
            return
        self.stop_event.set()
        try:
            self.board.rst()
        except Exception:
            pass
        self.board.close()
