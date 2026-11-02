#!/usr/bin/env python3
"""Print a centered 24x24 black/white checkerboard, then feed blank paper.

Requires Pinbridge WebUI running at ws://127.0.0.1:8765/ws and the printer
connected. The printhead data polarity (1=black) follows the current experimental
hypothesis. This is a low-level demo, not the stock four-phase row scheduler.
Paper-feed distance is mechanism-dependent: --tail-steps is half-step transitions,
not millimetres. Calibrate it on your printer before relying on a distance.
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
SQUARE_SIZE = 24
SQUARE_START = (HEAD_DOTS - SQUARE_SIZE) // 2


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


def make_row(y: int) -> bytes:
    """Return 48 bytes; checkerboard square is centered in the 384-dot head."""
    bits = [0] * HEAD_DOTS  # 0=white, based on the current polarity hypothesis
    for x in range(SQUARE_START, SQUARE_START + SQUARE_SIZE):
        bits[x] = int((x - SQUARE_START + y) % 2 == 0)  # 1=black
    return bytes(
        sum(bits[base + bit] << bit for bit in range(8))
        for base in range(0, HEAD_DOTS, 8)
    )


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    parser.add_argument("--port", help="serial port if WebUI is not connected yet")
    parser.add_argument("--bit-us", type=int, default=2)
    parser.add_argument("--hold-us", type=int, default=1)
    parser.add_argument("--latch-us", type=int, default=2)
    parser.add_argument("--heat-us", type=int, default=1500,
                        help="PB11 high pulse per row (0..6000 us; not a safety guarantee)")
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
            print(f"plan: {SQUARE_SIZE}x{SQUARE_SIZE} checkerboard centered at x={SQUARE_START}..{SQUARE_START + SQUARE_SIZE - 1}; ")
            print(f"24 heated rows at {args.heat_us} us per row, one half-step after each; then {args.tail_steps} unheated {args.tail_direction} half-steps of blank-paper feed")
            print("Reminder: tail half-steps do not map to mm until calibrated; row timing is experimental.")
            if args.dry_run:
                for y in range(SQUARE_SIZE):
                    print("".join("##" if ((x + y) % 2 == 0) else "  " for x in range(SQUARE_SIZE)))
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
                for y in range(SQUARE_SIZE):
                    row = make_row(y)
                    await con.printer(op="shift", data_hex=row.hex(), bit_delay_us=args.bit_us, data_hold_us=args.hold_us)
                    await con.printer(op="latch", pulse_us=args.latch_us)
                    await con.printer(op="heat", pulse_us=args.heat_us)
                    await con.printer(op="step", direction="FWD", count=1,
                                      inter_step_us=args.row_step_us, coil_on_us=args.coil_on_us)
                    print(f"printed row {y + 1}/{SQUARE_SIZE}")
                if args.tail_steps:
                    # No shift/latch/heat during this final feed: move only the paper.
                    await con.printer(op="step", direction=args.tail_direction,
                                      count=args.tail_steps, inter_step_us=args.row_step_us,
                                      coil_on_us=args.coil_on_us)
                    print(f"blank-paper feed complete: {args.tail_steps} half-steps (not calibrated mm)")
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
