#!/usr/bin/env python3
"""Move the Pinbridge stepper using the MCU-side official eight-state sequence.

--steps counts half-step transitions. --full-steps is a convenience expressed as
two half-step transitions per requested full-step angle; the firmware still runs
the same eight-state table. Timing uses a 1 ms nonblocking scheduler; actual motor speed remains hardware-dependent.

This script does not flash firmware or verify hardware. Use an owner-reviewed,
current-limited motor setup with the thermal head disconnected. The coils are
released by setting all four phase inputs low after the run.
"""

import argparse
import asyncio
import sys

import aiohttp

MOTOR_PINS = ("PA9", "PA10", "PC3", "PC2")
SUCCESS_TYPES = {"raw", "result", "connected"}
ERROR_TYPES = {"error", "confirm_required"}
DEFAULT_FULL_STEPS = 100
MAX_HALF_STEPS = 60000
MAX_RUN_US = 60_000_000


class Pinbridge:
    def __init__(self, websocket):
        self.websocket = websocket

    async def request(self, **message):
        await self.websocket.send_json(message)
        while True:
            reply = await asyncio.wait_for(self.websocket.receive_json(), timeout=70)
            kind = reply.get("type")
            if kind in ERROR_TYPES:
                raise RuntimeError(f"{kind}: {reply.get('message', reply)}")
            if kind in SUCCESS_TYPES:
                return reply

    async def raw(self, command):
        return await self.request(action="raw", command=command)

    async def step(self, direction, count, inter_step_us, coil_on_us):
        return await self.request(
            action="printer",
            confirm="printer",
            op="step",
            direction=direction,
            count=count,
            inter_step_us=inter_step_us,
            coil_on_us=coil_on_us,
        )


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    parser.add_argument(
        "--port", help="serial port to connect if the WebUI has not connected"
    )
    counts = parser.add_mutually_exclusive_group()
    counts.add_argument("--steps", type=int, help="number of half-step transitions")
    counts.add_argument(
        "--full-steps",
        type=int,
        help=f"number of full-step angles (two half-step transitions each; default {DEFAULT_FULL_STEPS})",
    )
    parser.add_argument(
        "--rate",
        type=float,
        default=1_000_000 / 6000,
        help="requested half-steps per second (default requests a 6000 us period; approximate)",
    )
    parser.add_argument(
        "--coil-on-us",
        type=int,
        default=1000,
        help="requested dwell for each phase in microseconds (default: 1000)",
    )
    parser.add_argument(
        "--reverse",
        action="store_true",
        help="traverse the eight-state sequence backwards",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="inspect status and print the plan without moving",
    )
    parser.add_argument(
        "--yes", action="store_true", help="skip the interactive STEP confirmation"
    )
    args = parser.parse_args()

    if args.steps is not None:
        count = args.steps
        count_description = f"{count} half-steps"
    else:
        full_steps = DEFAULT_FULL_STEPS if args.full_steps is None else args.full_steps
        if full_steps < 1:
            parser.error("--full-steps must be at least 1")
        count = 2 * full_steps
        count_description = f"{full_steps} full-step angles ({count} half-steps)"

    if not 1 <= count <= MAX_HALF_STEPS:
        parser.error(f"total half-step transitions must be 1..{MAX_HALF_STEPS}")
    if not 1 <= args.rate <= 1000:
        parser.error("--rate must be 1..1000 half-steps per second")
    if not 0 <= args.coil_on_us <= 60_000_000:
        parser.error("--coil-on-us must be 0..60000000")

    period_us = round(1_000_000 / args.rate)
    gap_us = period_us - args.coil_on_us
    if gap_us < 0:
        parser.error("--coil-on-us cannot exceed the requested step period")
    if count * period_us > MAX_RUN_US:
        parser.error("total requested run time must be at most 60 seconds")

    return args, count, count_description, period_us, gap_us


async def run(args, count, count_description, period_us, gap_us):
    timeout = aiohttp.ClientTimeout(total=None)
    async with aiohttp.ClientSession(timeout=timeout) as session:
        async with session.ws_connect(args.url, max_msg_size=0) as websocket:
            bridge = Pinbridge(websocket)
            try:
                status = await bridge.raw("STA " + " ".join(MOTOR_PINS))
            except RuntimeError as exc:
                if "Not connected" not in str(exc) or not args.port:
                    raise SystemExit(
                        f"Connect Pinbridge first (or supply --port): {exc}"
                    )
                await bridge.request(action="connect", port=args.port)
                status = await bridge.raw("STA " + " ".join(MOTOR_PINS))

            print("motor pin status:", status.get("reply"))
            print(
                f"plan: {count_description}; direction={'REV' if args.reverse else 'FWD'}; "
                f"requested period={period_us} us; coil dwell={args.coil_on_us} us; "
                f"inter-step gap={gap_us} us"
            )
            print("Note: firmware timing resolution is 1 ms; actual motor speed is not hardware-verified.")
            if args.dry_run:
                print("dry run: no motor phase or step commands sent")
                return

            if not args.yes:
                prompt = "Motor supply current-limited and thermal head disconnected? Type STEP to continue: "
                print(prompt, end="", flush=True)
                if (await asyncio.to_thread(sys.stdin.readline)).strip() != "STEP":
                    raise SystemExit("aborted")

            phases_configured = False
            try:
                for pin in MOTOR_PINS:
                    await bridge.request(action="mode", pin=pin, mode="lo", confirm=pin)
                phases_configured = True
                result = await bridge.step(
                    "REV" if args.reverse else "FWD",
                    count,
                    gap_us,
                    args.coil_on_us,
                )
                print("step result:", result.get("reply", result))
            finally:
                if phases_configured:
                    try:
                        await asyncio.shield(
                            bridge.raw(
                                "SET " + " ".join(f"{pin}=0" for pin in MOTOR_PINS)
                            )
                        )
                        print("motor phases released (all four inputs low)")
                    except Exception as exc:
                        print(
                            f"WARNING: unable to release motor phases: {exc}. "
                            "Stop the motor using the WebUI.",
                            file=sys.stderr,
                        )


def main():
    args, count, description, period_us, gap_us = parse_args()
    try:
        asyncio.run(run(args, count, description, period_us, gap_us))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)


if __name__ == "__main__":
    main()
