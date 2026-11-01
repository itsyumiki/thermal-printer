#!/usr/bin/env python3
"""Stepper demo through the pinbridge web console (WebSocket client).

Drives a DRV8833 (AIN1/AIN2/BIN1/BIN2 on PA9/PA10/PC3/PC2 per the owner's trace; UNVERIFIED on
hardware) for N full steps. The web console owns the USB serial port, so this script talks to its
/ws endpoint instead of opening the port itself.

    python stepper_ws_demo.py                       # 200 steps forward at 50 steps/s
    python stepper_ws_demo.py --steps 200 --rate 30 --reverse
    python stepper_ws_demo.py --dry-run             # go through the motions with no motor pins touched

Start the web console first and connect it to the board (browser or `--port` here).

SAFETY: motor supply current-limited and low voltage for the first run; print head disconnected.
A USB-CDC round trip is NOT real-time: expect millisecond jitter and no precise acceleration.
If the WebSocket drops mid-run this script cannot de-energise the coils; the board watchdog will NOT
help while the web console is alive (it keeps feeding it). Stop the motor from the web UI then.
"""

import argparse
import asyncio
import sys
import time

import aiohttp

# Pin roles (A1, A2, B1, B2). Matches pins.toml notes: PC3 = phase B1, PC2 = phase B2.
PINS = ("PA9", "PA10", "PC3", "PC2")
# Full-step, two coils on: exactly one input high per coil, so a bridge is never braked or shorted.
STEPS = ((1, 0, 1, 0), (0, 1, 1, 0), (0, 1, 0, 1), (1, 0, 0, 1))
OK_TYPES = {"raw", "result", "connected"}
FAIL_TYPES = {"error", "confirm_required"}


class Console:
    def __init__(self, ws):
        self.ws = ws

    async def call(self, **msg):
        """Send one request, return the one matching reply; ignore broadcasts (event/hello/...)."""
        await self.ws.send_json(msg)
        while True:
            reply = await asyncio.wait_for(self.ws.receive_json(), timeout=5)
            kind = reply.get("type")
            if kind in FAIL_TYPES:
                raise RuntimeError(f"{kind}: {reply.get('message', reply)}")
            if kind in OK_TYPES:
                return reply

    async def raw(self, line):
        return await self.call(action="raw", command=line)


def step_line(pattern):
    return "SET " + " ".join(f"{pin}={v}" for pin, v in zip(PINS, pattern))


async def coast(con):
    """All four inputs low = DRV8833 coast. Pins stay outputs (low) so the state is deterministic."""
    await con.raw(step_line((0, 0, 0, 0)))


async def run(args):
    timeout = aiohttp.ClientTimeout(total=None)
    async with aiohttp.ClientSession(timeout=timeout) as session:
        async with session.ws_connect(args.url, max_msg_size=0) as ws:
            con = Console(ws)
            # The first message is the server's "hello" broadcast; call() skips it.
            try:
                sta = await con.raw("STA " + " ".join(PINS))
            except RuntimeError as exc:
                if "Not connected" in str(exc) and args.port:
                    await con.call(action="connect", port=args.port)
                    sta = await con.raw("STA " + " ".join(PINS))
                else:
                    raise SystemExit(
                        f"Console not connected to a board ({exc}). Use --port or the web UI."
                    )
            print("pin state before:", sta.get("reply"))
            direction = -1 if args.reverse else 1
            print(
                f"{args.steps} steps, {'reverse' if args.reverse else 'forward'}, {args.rate:g} steps/s, pins {PINS}"
            )
            if args.dry_run:
                print("dry run: no commands sent beyond STA")
                return
            if not args.yes:
                print(
                    "Motor supply current-limited? Head disconnected? Type STEP to start: ",
                    end="",
                    flush=True,
                )
                if (await asyncio.to_thread(sys.stdin.readline)).strip() != "STEP":
                    raise SystemExit("aborted")

            energised = False
            try:
                for (
                    pin
                ) in PINS:  # goes through the console's dangerous-pin confirmation path
                    await con.call(action="mode", pin=pin, mode="lo", confirm=pin)
                energised = True
                period = 1.0 / args.rate
                start = nxt = time.monotonic()
                worst = 0.0
                for i in range(args.steps):
                    await con.raw(step_line(STEPS[(i * direction) % 4]))
                    nxt += period
                    delay = nxt - time.monotonic()
                    worst = max(worst, -delay)
                    if delay > 0:
                        await asyncio.sleep(delay)
                took = time.monotonic() - start
                print(
                    f"done: {args.steps} steps in {took:.2f}s ({args.steps / took:.1f}/s), worst lateness {worst * 1000:.0f} ms"
                )
            finally:
                if energised:
                    try:
                        await asyncio.shield(coast(con))
                        print("coils released (all inputs low = coast)")
                    except (
                        Exception
                    ) as exc:  # report loudly: the motor may still be energised
                        print(
                            f"!!! COULD NOT COAST: {exc}. Stop the motor from the web UI NOW.",
                            file=sys.stderr,
                        )


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    ap.add_argument(
        "--port", help="serial port, only if the console is not connected yet"
    )
    ap.add_argument("--steps", type=int, default=200)
    ap.add_argument(
        "--rate", type=float, default=50.0, help="steps per second (default 50)"
    )
    ap.add_argument("--reverse", action="store_true")
    ap.add_argument("--yes", action="store_true", help="skip the typed confirmation")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if not (1 <= args.steps <= 100000) or not (1 <= args.rate <= 500):
        sys.exit("steps must be 1..100000 and rate 1..500")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)


if __name__ == "__main__":
    main()
