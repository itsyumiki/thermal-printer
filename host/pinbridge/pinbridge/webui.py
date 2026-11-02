"""Pinbridge local web UI and WebSocket API."""

from __future__ import annotations

import asyncio
import json
import threading
import webbrowser
from pathlib import Path

from aiohttp import WSMsgType, web

from .board import Board
from .fake import FakeSerial
from .pins_gen import CFG_HASH, DENIED, PINS

ASSETS = Path(__file__).with_name("web")


def _ports():
    try:
        from serial.tools import list_ports

        return [
            {"device": p.device, "description": p.description or p.device}
            for p in list_ports.comports()
        ]
    except Exception:
        return []


def _pin_data():
    rows = []
    for p in PINS:
        name, port, bit, caps, adc, pwm, alias, note = p
        rows.append(
            {
                "name": name,
                "port": "ABC"[port],
                "bit": bit,
                "caps": list(caps),
                "adc": adc,
                "alias": alias,
                "note": note,
                "dangerous": name
                in {
                    "PA0",
                    "PA1",
                    "PA9",
                    "PA10",
                    "PB8",
                    "PB9",
                    "PB10",
                    "PB11",
                    "PC2",
                    "PC3",
                },
            }
        )
    for name, note in DENIED.items():
        rows.append(
            {
                "name": name,
                "port": "SYS",
                "bit": None,
                "caps": [],
                "alias": None,
                "note": note,
                "denied": True,
                "dangerous": False,
            }
        )
    return sorted(
        rows,
        key=lambda x: (
            {"A": 0, "B": 1, "C": 2, "SYS": 3}[x["port"]],
            x["bit"] if x["bit"] is not None else 99,
            x["name"],
        ),
    )


class WebBackend:
    def __init__(self, port=None, mock=False):
        self.port = port
        self.mock = mock
        self.board = None
        self.board_lock = threading.RLock()
        self.clients = set()
        self.loop = None
        self.last_event = None

    async def broadcast(self, payload):
        dead = []
        message = json.dumps(payload)
        for ws in tuple(self.clients):
            try:
                await ws.send_str(message)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)

    def _board_event(self, event):
        if self.loop and not self.loop.is_closed():
            asyncio.run_coroutine_threadsafe(
                self.broadcast(
                    {
                        "type": "event",
                        "event": str(event[0]),
                        "detail": str(event[1]) if len(event) > 1 else "",
                    }
                ),
                self.loop,
            )

    async def _status(self):
        if not self.board:
            return {"connected": False, "port": self.port, "pins": [], "uptime": None}

        def read():
            with self.board_lock:
                modes = self.board.sta()
                values = {}
                readable = []
                for part in modes:
                    if "=" in part:
                        name, mode = part.split("=", 1)
                        if mode in {"in", "up", "dn", "lo", "hi"}:
                            readable.append(name)
                if readable:
                    try:
                        for part in self.board.get(*readable):
                            if "=" in part:
                                k, v = part.split("=", 1)
                                values[k] = int(v)
                    except Exception:
                        pass
                return modes, values, self.board.up

        try:
            modes, values, up = await asyncio.to_thread(read)
            parsed = {}
            for item in modes:
                if "=" in item:
                    k, v = item.split("=", 1)
                    parsed[k] = v
            return {
                "connected": True,
                "port": self.port or ("mock" if self.mock else None),
                "pins": [
                    {"name": k, "mode": v, "value": values.get(k)}
                    for k, v in parsed.items()
                ],
                "uptime": up,
            }
        except Exception as exc:
            return {
                "connected": True,
                "port": self.port,
                "error": str(exc),
                "pins": [],
                "uptime": self.board.up,
            }

    async def index(self, request):
        return web.FileResponse(ASSETS / "index.html")

    async def asset(self, request):
        name = request.match_info["name"]
        if name not in {"app.js", "style.css"}:
            raise web.HTTPNotFound()
        return web.FileResponse(ASSETS / name)

    async def api_info(self, request):
        return web.json_response(
            {
                "pins": _pin_data(),
                "denied": DENIED,
                "cfg_hash": CFG_HASH,
                "ports": _ports(),
                "connected": bool(self.board),
                "mock": self.mock,
            }
        )

    async def _reply(self, ws, **kwargs):
        await ws.send_json(kwargs)

    async def _execute(self, msg):
        action = msg.get("action")
        if action == "ports":
            return {"type": "ports", "ports": _ports()}
        if action == "connect":
            if self.board:
                return {
                    "type": "error",
                    "message": "Already connected. Disconnect first.",
                }
            port = msg.get("port")
            if not self.mock and not port:
                return {"type": "error", "message": "Choose a serial port first."}

            def open_board():
                b = Board(port, serial_obj=FakeSerial() if self.mock else None)
                b.on_event = self._board_event
                b.open()
                return b

            try:
                self.board = await asyncio.to_thread(open_board)
                self.port = port
                status = await self._status()
                return {"type": "connected", "status": status}
            except Exception as exc:
                self.board = None
                return {"type": "error", "message": str(exc)}
        if action == "disconnect":
            b, self.board = self.board, None
            if b:
                await asyncio.to_thread(b.close)
            return {"type": "disconnected"}
        if action == "status":
            return {"type": "status", "status": await self._status()}
        if action == "printer_poll":
            if not self.board:
                return {"type": "error", "message": "Connect to a device first."}
            try:

                def poll_printer():
                    with self.board_lock:
                        # Keep the two digital sensors in their measured modes and
                        # the two analog sensor nets connected to the SDK ADC scanner.
                        self.board.command("MOD PB1 up")
                        self.board.command("MOD PB3 in")
                        self.board.command("MOD PC4 adc")
                        self.board.command("MOD PC5 adc")
                        digital = self.board.get("PB1", "PB3")
                        try:
                            analog = self.board.adc("PC4", "PC5")
                        except BusyError:
                            # First ADC mode switch registers channels with the SDK's
                            # periodic scanner; allow it to collect before retrying.
                            time.sleep(0.12)
                            analog = self.board.adc("PC4", "PC5")
                    levels = {}
                    for item in digital:
                        if "=" in item:
                            key, value = item.split("=", 1)
                            levels[key] = int(value)
                    readings = {}
                    for item in analog:
                        if "=" in item:
                            key, value = item.split("=", 1)
                            raw, mv = value.split("/", 1)
                            readings[key] = {"raw": int(raw), "mv": int(mv)}
                    return levels, readings

                levels, readings = await asyncio.to_thread(poll_printer)
                status = await self._status()
                for pin in status["pins"]:
                    reading = readings.get(pin["name"])
                    if reading:
                        pin["value"] = reading["raw"]
                        pin["mv"] = reading["mv"]
                return {
                    "type": "printer_status",
                    "status": status,
                    "printer": {
                        "paper_present": levels.get("PB3") == 0,
                        "paper_level": levels.get("PB3"),
                        "power_button_pressed": levels.get("PB1") == 0,
                        "power_button_level": levels.get("PB1"),
                        "power_latched": any(
                            p["name"] == "PA1" and p["value"] == 1
                            for p in status["pins"]
                        ),
                        "head_sensor": readings.get("PC4"),
                        "battery_sensor": readings.get("PC5"),
                    },
                }
            except Exception as exc:
                return {
                    "type": "error",
                    "message": f"Printer status poll failed: {exc}",
                }
        if action in {"startup", "power"}:
            if not self.board:
                return {"type": "error", "message": "Connect to a device first."}
            try:
                turn_on = action == "startup" or msg.get("value") in (
                    1,
                    True,
                    "1",
                    "on",
                    "ON",
                )

                def run_power_sequence():
                    with self.board_lock:
                        # PA1 is the power latch. Keep the sequence atomic against
                        # other WebSocket clients issuing serial commands.
                        self.board.command("MOD PA1 hi")
                        if turn_on:
                            self.board.command("SET PA1=1")
                            # PB1 is the power-button sense net; stock hardware uses pull-up.
                            self.board.command("MOD PB1 up")
                            return ["ok power=on", "ok button=ready"]
                        return self.board.command("SET PA1=0")

                reply = await asyncio.to_thread(run_power_sequence)
                return {
                    "type": "result",
                    "action": "startup" if action == "startup" else "power",
                    "pin": "PA1",
                    "reply": reply,
                    "status": await self._status(),
                }
            except Exception as exc:
                return {"type": "error", "message": f"Power sequence failed: {exc}"}
        if action == "printer":
            # Explicit high-level WebSocket facade; host-side validation bounds demo commands.
            if not self.board:
                return {"type": "error", "message": "Connect to a device first."}
            if msg.get("confirm") != "printer":
                return {"type": "confirm_required", "message": "Confirm printer-head/stepper operation.", "confirm": "printer"}
            op = str(msg.get("op", "")).lower()
            try:
                def integer(key, low, high):
                    value = msg.get(key)
                    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
                        raise ValueError(f"{key} must be an integer in {low}..{high}")
                    return value
                if op == "shift":
                    data = str(msg.get("data_hex", ""))
                    if len(data) != 96 or any(ch not in "0123456789abcdefABCDEF" for ch in data):
                        raise ValueError("data_hex must contain exactly 96 hexadecimal characters (48 bytes)")
                    bit_us = integer("bit_delay_us", 0, 1000)
                    hold_us = integer("data_hold_us", 0, 1000)
                    line = f"PHEAD {data} {bit_us} {hold_us}"
                elif op == "latch":
                    line = f"PLATCH {integer('pulse_us', 0, 1000)}"
                elif op == "heat":
                    line = f"PHEAT {integer('pulse_us', 0, 6000)}"
                elif op == "step":
                    direction = str(msg.get("direction", "")).upper()
                    if direction not in {"FWD", "REV"}:
                        raise ValueError("direction must be FWD or REV")
                    count = integer("count", 0, 60000)
                    inter = integer("inter_step_us", 0, 60000000)
                    dwell = integer("coil_on_us", 0, 60000000)
                    if count * (((inter + 999) // 1000) + max(1, (dwell + 999) // 1000)) > 60000:
                        raise ValueError("rounded 1 ms step timing must be <= 60,000 ms")
                    line = f"PSTEP {direction} {count} {inter} {dwell}"
                    started = await self._command(line)
                    if started and started[0] == "started":
                        deadline = asyncio.get_running_loop().time() + 65
                        while True:
                            state = await self._command("PSTEPSTAT")
                            if state and state[0] == "done":
                                return {"type": "raw", "command": line, "reply": ["ok", "halfsteps"]}
                            if not state or state[0] != "busy":
                                raise RuntimeError(f"unexpected stepper status: {state}")
                            if asyncio.get_running_loop().time() >= deadline:
                                raise TimeoutError("stepper did not finish within 65 seconds")
                            await asyncio.sleep(0.01)
                else:
                    raise ValueError("op must be shift, latch, heat, or step")
                return {"type": "raw", "command": line, "reply": await self._command(line)}
            except ValueError as exc:
                return {"type": "error", "message": str(exc)}
            except Exception as exc:
                return {"type": "error", "message": f"Printer command failed: {exc}"}
        if action == "raw":
            line = str(msg.get("command", "")).strip()
            if not line or len(line) > 120 or any(ord(c) < 32 for c in line):
                return {
                    "type": "error",
                    "message": "Command must be 1–120 printable characters.",
                }
            return {"type": "raw", "command": line, "reply": await self._command(line)}
        if not self.board:
            return {"type": "error", "message": "Connect to a device first."}
        pin = str(msg.get("pin", "")).upper()
        allowed = {p[0] for p in PINS}
        if action in {"mode", "set", "get", "adc", "listen"} and pin not in allowed:
            return {"type": "error", "message": "Unknown or unavailable GPIO pin."}
        try:
            if action == "mode":
                mode = str(msg.get("mode", "")).lower()
                valid = {"hiz", "in", "up", "dn", "lo", "hi", "adc"}
                if mode not in valid:
                    return {"type": "error", "message": "Unsupported mode."}
                spec = next(p for p in PINS if p[0] == pin)
                need = {
                    "in": "in",
                    "up": "pu",
                    "dn": "pd",
                    "lo": "out",
                    "hi": "out",
                    "adc": "adc",
                }
                if mode != "hiz" and need.get(mode) not in spec[3]:
                    return {
                        "type": "error",
                        "message": f"{pin} does not support {mode}.",
                    }
                if (
                    mode in {"lo", "hi"}
                    and spec[0]
                    in {
                        "PA0",
                        "PA1",
                        "PA9",
                        "PA10",
                        "PB8",
                        "PB9",
                        "PB10",
                        "PB11",
                        "PC2",
                        "PC3",
                    }
                    and msg.get("confirm") != pin
                ):
                    return {
                        "type": "confirm_required",
                        "pin": pin,
                        "mode": mode,
                        "message": f"Confirm output mode for {pin}.",
                    }
                reply = await self._command(f"MOD {pin} {mode}")
            elif action == "set":
                value = 1 if msg.get("value") in (1, True, "1", "HIGH", "high") else 0
                spec = next(p for p in PINS if p[0] == pin)
                if "out" not in spec[3]:
                    return {
                        "type": "error",
                        "message": f"{pin} is not an output-capable pin.",
                    }
                if (
                    spec[0]
                    in {
                        "PA0",
                        "PA1",
                        "PA9",
                        "PA10",
                        "PB8",
                        "PB9",
                        "PB10",
                        "PB11",
                        "PC2",
                        "PC3",
                    }
                    and msg.get("confirm") != pin
                ):
                    return {
                        "type": "confirm_required",
                        "pin": pin,
                        "mode": "set",
                        "value": value,
                        "message": f"Confirm changing output on {pin}.",
                    }
                reply = await self._command(f"SET {pin}={value}")
            elif action == "get":
                # GET requires a configured pin. Prepare safe, non-power/non-driver
                # pins as inputs on first read; never silently reconfigure PA1 or
                # stepper/printhead outputs just to satisfy a Read click.
                current = {
                    p["name"]: p["mode"] for p in (await self._status())["pins"]
                }.get(pin, "off")
                if current in {"off", "hiz"}:
                    if pin in {
                        "PA0",
                        "PA1",
                        "PA9",
                        "PA10",
                        "PB8",
                        "PB9",
                        "PB10",
                        "PB11",
                        "PC2",
                        "PC3",
                    }:
                        return {
                            "type": "error",
                            "message": f"{pin} is not configured. Set its mode explicitly before reading to avoid changing a power/driver pin.",
                        }
                    mode = "up" if pin == "PB1" else "in"
                    await self._command(f"MOD {pin} {mode}")
                reply = await self._command(f"GET {pin}")
            elif action == "adc":
                spec = next(p for p in PINS if p[0] == pin)
                if "adc" not in spec[3]:
                    return {"type": "error", "message": f"{pin} does not support ADC."}
                current = {
                    p["name"]: p["mode"] for p in (await self._status())["pins"]
                }.get(pin, "off")
                if current != "adc":
                    await self._command(f"MOD {pin} adc")
                reply = await self._command(f"ADC {pin}")
            elif action == "listen":
                reply = await self._command(
                    f"LSN {pin}" if msg.get("enabled") else f"LSN off {pin}"
                )
            elif action == "reset":
                reply = await self._command("RST")
            elif action == "watchdog":
                ms = int(msg.get("ms", 5000))
                if ms != 0 and not 100 <= ms <= 3600000:
                    return {
                        "type": "error",
                        "message": "Watchdog must be 0 or 100–3600000 ms.",
                    }
                reply = await self._command(f"WDG {ms}")
            else:
                return {"type": "error", "message": f"Unknown action: {action}"}
            status = await self._status()
            if action == "adc" and reply:
                for item in reply:
                    if "=" not in item:
                        continue
                    name, reading = item.split("=", 1)
                    if "/" not in reading:
                        continue
                    raw, mv = reading.split("/", 1)
                    for pin_state in status["pins"]:
                        if pin_state["name"] == name:
                            pin_state["value"] = int(raw)
                            pin_state["mv"] = int(mv)
            return {
                "type": "result",
                "action": action,
                "pin": pin,
                "reply": reply,
                "status": status,
            }
        except Exception as exc:
            return {"type": "error", "message": str(exc)}

    async def _command(self, line):
        if not self.board:
            raise RuntimeError("Not connected")

        def run():
            with self.board_lock:
                return self.board.command(line)

        return await asyncio.to_thread(run)

    async def websocket(self, request):
        ws = web.WebSocketResponse(heartbeat=25, max_msg_size=16_384)
        await ws.prepare(request)
        self.clients.add(ws)
        self.loop = asyncio.get_running_loop()
        await ws.send_json(
            {
                "type": "hello",
                "pins": _pin_data(),
                "cfg_hash": CFG_HASH,
                "ports": _ports(),
                "mock": self.mock,
                "status": await self._status(),
            }
        )
        try:
            async for packet in ws:
                if packet.type == WSMsgType.TEXT:
                    try:
                        msg = json.loads(packet.data)
                        if not isinstance(msg, dict):
                            raise ValueError("Message must be a JSON object")
                        result = await self._execute(msg)
                    except Exception as exc:
                        result = {"type": "error", "message": str(exc)}
                    await ws.send_json(result)
                elif packet.type == WSMsgType.ERROR:
                    break
        finally:
            self.clients.discard(ws)
        return ws

    async def cleanup(self, app):
        if self.board:
            b, self.board = self.board, None
            await asyncio.to_thread(b.close)


def create_app(backend):
    app = web.Application()
    app.router.add_get("/", backend.index)
    app.router.add_get("/assets/{name}", backend.asset)
    app.router.add_get("/api/info", backend.api_info)
    app.router.add_get("/ws", backend.websocket)
    app.on_cleanup.append(backend.cleanup)
    return app


def run_webui(
    port=None, mock=False, host="127.0.0.1", http_port=8765, open_browser=True
):
    backend = WebBackend(port=port, mock=mock)
    app = create_app(backend)
    url = f"http://{host}:{http_port}/"

    async def launch():
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, host, http_port)
        await site.start()
        print(f"Pinbridge WebUI: {url}", flush=True)
        if open_browser:
            webbrowser.open(url)
        while True:
            await asyncio.sleep(3600)

    try:
        asyncio.run(launch())
    except KeyboardInterrupt:
        pass
