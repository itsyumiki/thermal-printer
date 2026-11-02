#!/usr/bin/env python3
"""Print 24 solid-black rows across all 384 printhead dots, then feed blank paper.

Requires Pinbridge WebUI running at ws://127.0.0.1:8765/ws and the printer
connected. The printhead data polarity (1=black) follows the current experimental
hypothesis. PB8 is an active-low latch and must idle high during shifting. This is a low-level demo, not the stock four-phase row scheduler.
Paper-feed distance is mechanism-dependent: --tail-steps is phase transitions,
not millimetres. The firmware PSTEP count includes the initial phase, so one
actual transition requires count=2. Calibrate feed distance before relying on mm.
"""
import argparse
import asyncio
import sys

import aiohttp

HEAD_PINS = ("PB8", "PB9", "PB10", "PB11")
MOTOR_PINS = ("PA9", "PA10", "PC3", "PC2")
OK_TYPES = {"raw", "result", "connected"}
FAIL_TYPES = {"error", "confirm_required"}
HEAD_DOTS = 384
ROW_COUNT = 24


class Console:
    def __init__(self, ws):
        self.ws = ws

    async def call(self, **msg):
        await self.ws.send_json(msg)
        while True:
            reply = await asyncio.wait_for(self.ws.receive_json(), timeout=70)
            kind = reply.get("type")
            if kind in FAIL_TYPES:
                raise RuntimeError(f"{kind}: {reply.get('message', reply)}")
            if kind in OK_TYPES:
                return reply

    async def raw(self, command):
        return await self.call(action="raw", command=command)

    async def printer(self, **kwargs):
        return await self.call(action="printer", confirm="printer", **kwargs)


def make_row(y: int = 0) -> bytes:
    """Return 48 bytes with all 384 dots selected as black (1 bits)."""
    del y  # Keep the row argument for callers; every row is identical.
    return b"\xff" * (HEAD_DOTS // 8)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    parser.add_argument("--port", help="serial port if WebUI is not connected yet")
    parser.add_argument("--bit-us", type=int, default=2)
    parser.add_argument("--hold-us", type=int, default=1)
    parser.add_argument("--latch-us", type=int, default=2)
    parser.add_argument("--heat-us", type=int, default=6000,
                        help="PB11 high pulse per row (0..6000 us; not a safety guarantee)")
    parser.add_argument("--row-steps", type=int, default=8,
                        help="actual motor phase transitions after each printed row (default: 8)")
    parser.add_argument("--row-step-us", type=int, default=1000,
                        help="requested inter-step gap for each printed row")
    parser.add_argument("--coil-on-us", type=int, default=1000)
    parser.add_argument("--tail-steps", type=int, default=24,
                        help="blank-paper feed after square, in half-step transitions (default: 24; calibrate to mm)")
    parser.add_argument("--tail-direction", choices=("FWD", "REV"), default="FWD")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--yes", action="store_true", help="skip typed PRINT confirmation")
    args = parser.parse_args()
    for name in ("bit_us", "hold_us", "latch_us"):
        if not 0 <= getattr(args, name) <= 1000:
            parser.error(f"--{name.replace('_', '-')} must be 0..1000")
    if not 0 <= args.heat_us <= 6000:
        parser.error("--heat-us must be 0..6000")
    if not 0 <= args.row_step_us <= 60_000_000 or not 0 <= args.coil_on_us <= 60_000_000:
        parser.error("step timing must be 0..60000000 us")
    if not 0 <= args.row_steps <= 60000:
        parser.error("--row-steps must be 0..60000")
    if not 0 <= args.tail_steps <= 60000:
        parser.error("--tail-steps must be 0..60000")
    return args


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
            print(f"plan: {ROW_COUNT} solid-black rows across all {HEAD_DOTS} dots (48 bytes of 0xFF per row)")
            print(f"{ROW_COUNT} heated rows at {args.heat_us} us per row, {args.row_steps} phase transitions after each; then {args.tail_steps} unheated {args.tail_direction} phase transitions of blank-paper feed")
            print("Reminder: phase transitions do not map to mm until calibrated; row timing is experimental.")
            if args.dry_run:
                print(f"dry-run row payload: {make_row().hex()} ({len(make_row())} bytes, all bits set)")
                print("dry run: no GPIO, printhead, heater, or step commands sent")
                return
            if not args.yes:
                print("This energises the printhead heater and stepper. Type PRINT to continue: ", end="", flush=True)
                if (await asyncio.to_thread(sys.stdin.readline)).strip() != "PRINT":
                    raise SystemExit("aborted")

            configured = False
            try:
                for pin in HEAD_PINS + MOTOR_PINS:
                    await con.call(action="mode", pin=pin, mode="lo", confirm=pin)
                configured = True
                # The head latch is active-low and must idle HIGH while bits shift.
                # Leaving PB8 at the mode-command default (LOW) can expose shifting
                # data continuously instead of holding the completed 384-bit row.
                await con.raw("SET PB8=1 PB9=0 PB10=0 PB11=0")
                for y in range(ROW_COUNT):
                    row = make_row(y)
                    await con.printer(op="shift", data_hex=row.hex(), bit_delay_us=args.bit_us, data_hold_us=args.hold_us)
                    await con.printer(op="latch", pulse_us=args.latch_us)
                    await con.printer(op="heat", pulse_us=args.heat_us)
                    # PSTEP count includes the initial phase, so N transitions need N+1.
                    await con.printer(op="step", direction="FWD", count=args.row_steps + 1,
                                      inter_step_us=args.row_step_us, coil_on_us=args.coil_on_us)
                    print(f"printed row {y + 1}/{ROW_COUNT}; fed {args.row_steps} phase transitions")
                if args.tail_steps:
                    # No shift/latch/heat during this final feed: move only the paper.
                    # N requested transitions require N+1 phase entries in PSTEP.
                    await con.printer(op="step", direction=args.tail_direction,
                                      count=args.tail_steps + 1, inter_step_us=args.row_step_us,
                                      coil_on_us=args.coil_on_us)
                    print(f"blank-paper feed complete: {args.tail_steps} phase transitions (not calibrated mm)")
            finally:
                if configured:
                    try:
                        await asyncio.shield(con.raw("SET " + " ".join(f"{pin}=0" for pin in MOTOR_PINS)))
                        print("motor phases released")
                    except Exception as exc:
                        print(f"WARNING: unable to release motor phases: {exc}; use the WebUI", file=sys.stderr)
            print("done")


def main():
    args = parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)


if __name__ == "__main__":
    main()
