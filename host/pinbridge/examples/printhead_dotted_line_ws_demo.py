#!/usr/bin/env python3
"""Print one 384-dot row as alternating 24-white / 24-black blocks via Pinbridge WS.

Start `python -m pinbridge webui`, connect the board, then run this script.
The MCU executes the shift/latch/strobe/step timing; USB/WebSocket latency is not
used to time individual GPIO edges. This is an experimental low-level test, not
an implementation of the stock four-phase row-printing scheduler.
"""
import argparse
import asyncio
import sys

import aiohttp

HEAD_PINS = ("PB8", "PB9", "PB10", "PB11")
MOTOR_PINS = ("PA9", "PA10", "PC3", "PC2")
OK_TYPES = {"raw", "result", "connected"}
FAIL_TYPES = {"error", "confirm_required"}


class Console:
    def __init__(self, ws):
        self.ws = ws

    async def call(self, **msg):
        await self.ws.send_json(msg)
        while True:
            reply = await asyncio.wait_for(self.ws.receive_json(), timeout=8)
            kind = reply.get("type")
            if kind in FAIL_TYPES:
                raise RuntimeError(f"{kind}: {reply.get('message', reply)}")
            if kind in OK_TYPES:
                return reply

    async def raw(self, command):
        return await self.call(action="raw", command=command)

    async def printer(self, **kwargs):
        return await self.call(action="printer", confirm="printer", **kwargs)


def make_row():
    bits = [int((i // 24) % 2 == 1) for i in range(384)]  # LSB-first; 1=black per current hypothesis
    return bytes(sum(bits[base + bit] << bit for bit in range(8)) for base in range(0, 384, 8))


async def run(args):
    async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=None)) as session:
        async with session.ws_connect(args.url, max_msg_size=0) as ws:
            con = Console(ws)
            try:
                status = await con.raw("STA " + " ".join(HEAD_PINS + MOTOR_PINS))
            except RuntimeError as exc:
                if "Not connected" not in str(exc) or not args.port:
                    raise SystemExit(f"Connect Pinbridge first (or use --port): {exc}")
                await con.call(action="connect", port=args.port)
                status = await con.raw("STA " + " ".join(HEAD_PINS + MOTOR_PINS))
            print("pins before:", status.get("reply"))
            row = make_row()
            print(f"row={len(row)} bytes, pattern=24 white / 24 black, strobe={args.heat_us} us")
            if args.dry_run:
                print("dry run: no GPIO/print commands sent")
                return
            if not args.yes:
                print("Heater is connected and this will energise it. Type PRINT to continue: ", end="", flush=True)
                if (await asyncio.to_thread(sys.stdin.readline)).strip() != "PRINT":
                    raise SystemExit("aborted")
            # Use the normal WebSocket confirmation flow to configure each driver pin.
            for pin in HEAD_PINS + MOTOR_PINS:
                await con.call(action="mode", pin=pin, mode="lo", confirm=pin)
            print("shift:", (await con.printer(op="shift", data_hex=row.hex(), bit_delay_us=args.bit_us, data_hold_us=args.hold_us)).get("reply"))
            print("latch:", (await con.printer(op="latch", pulse_us=args.latch_us)).get("reply"))
            print("heat:", (await con.printer(op="heat", pulse_us=args.heat_us)).get("reply"))
            if args.feed:
                print("step:", (await con.printer(op="step", direction="FWD", count=1, inter_step_us=args.step_gap_us, coil_on_us=args.coil_on_us)).get("reply"))
            else:
                print("paper feed skipped; row may overlap if another row is printed")
            print("done; outputs remain in their last commanded state. Use the UI/RST to return them to safe state.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    ap.add_argument("--port", help="serial port if the WebUI is not connected yet")
    ap.add_argument("--bit-us", type=int, default=2, help="delay on each clock half-cycle (0..1000 us)")
    ap.add_argument("--hold-us", type=int, default=1, help="data setup/hold before clock (0..1000 us)")
    ap.add_argument("--latch-us", type=int, default=2, help="PB8 low pulse duration (0..1000 us)")
    ap.add_argument("--heat-us", type=int, default=1500, help="PB11 high pulse (0..6000 us; 1500 us is not a safety guarantee)")
    ap.add_argument("--feed", action="store_true", help="advance one full step after printing")
    ap.add_argument("--step-gap-us", type=int, default=1000)
    ap.add_argument("--coil-on-us", type=int, default=1000)
    ap.add_argument("--yes", action="store_true", help="skip typed confirmation")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if not (0 <= args.bit_us <= 1000 and 0 <= args.hold_us <= 1000 and 0 <= args.latch_us <= 1000 and 0 <= args.heat_us <= 6000):
        sys.exit("timings outside firmware limits")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)


if __name__ == "__main__":
    main()
