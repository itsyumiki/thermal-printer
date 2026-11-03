#!/usr/bin/env python3
"""Print a 16x16 black/white checkerboard of 24 px boxes (384x384 dots).

Requires Pinbridge WebUI running at ws://127.0.0.1:8765/ws and the printer
connected. Head data polarity (1=black) is still the experimental hypothesis.
PB8 is an active-low latch and must idle high while shifting.

The job represents each logical image row as four identical TBP ROW events,
matching the stock firmware's statically inferred four-event accounting.

The demo submits all four stock-style events per logical row in one print-job
request. The Bun host keeps the advertised TBP queue window full and consumes
terminal statuses. Sprint 4 still serializes row preparation and motion; this is
not a claim of continuous paper motion or hardware-validated timing.

Limits (read these):
  * Stock timings and four-event accounting are static-analysis-derived, not
    measured on this printer.
  * Heater use passes through Sprint 3 sensor, temperature, battery, paper,
    per-slice and duty gates. Limits remain owner decisions, not bench validated.
  * No local firmware build, flash, or hardware validation is performed here.
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
BOX_PX = 24
BOXES = 16
ROW_COUNT = BOX_PX * BOXES  # 384 rows
EVENTS_PER_ROW = 4  # stock: four events per logical row
assert BOX_PX * BOXES == HEAD_DOTS and BOX_PX % 8 == 0


class Console:
    def __init__(self, ws):
        self.ws = ws

    async def batch(self, msgs):
        """Send several messages back-to-back, then collect the replies in order."""
        for msg in msgs:
            await self.ws.send_json(msg)
        replies = []
        while len(replies) < len(msgs):
            reply = await asyncio.wait_for(self.ws.receive_json(), timeout=70)
            kind = reply.get("type")
            if kind in FAIL_TYPES:
                raise RuntimeError(f"{kind}: {reply.get('message', reply)}")
            if kind == "print-progress":
                print(f"terminal rows: {reply.get('done')}/{reply.get('total')}")
                continue
            if kind == "job-status":
                continue
            if kind in OK_TYPES:
                replies.append(reply)
        return replies

    async def call(self, **msg):
        return (await self.batch([msg]))[0]

    async def raw(self, command):
        return await self.call(action="raw", command=command)

    async def printer(self, **kwargs):
        return await self.call(action="printer", confirm="printer", **kwargs)


def make_row(y: int) -> bytes:
    """48 bytes for dot-row y: 16 boxes of 24 dots, checkerboard, top-left black."""
    by = y // BOX_PX
    out = bytearray()
    for bx in range(BOXES):
        out += (b"\xff" if (bx + by) % 2 == 0 else b"\x00") * (BOX_PX // 8)
    return bytes(out)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    parser.add_argument("--port", help="serial port if WebUI is not connected yet")
    parser.add_argument("--bit-us", type=int, default=2)
    parser.add_argument("--hold-us", type=int, default=1)
    parser.add_argument("--latch-us", type=int, default=2)
    parser.add_argument(
        "--heat-us",
        type=int,
        default=2000,
        help="requested gated burn per ROW event (0..2500 us; owner limits are unmeasured)",
    )
    parser.add_argument(
        "--event-ms",
        type=int,
        default=6,
        help="print event / half-step period in ms (stock minimum: 2 ms tick x 3 = 6)",
    )
    parser.add_argument(
        "--steps-per-event",
        type=int,
        default=2,
        help="motor half-step transitions after each print event (default: 2; stock is 1)",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--profile",
        action="store_true",
        help="time each command's round trip (blank row, 0 us heat, no motion) and exit",
    )
    parser.add_argument(
        "--yes", action="store_true", help="skip typed PRINT confirmation"
    )
    args = parser.parse_args()
    for name in ("bit_us", "hold_us", "latch_us"):
        if not 0 <= getattr(args, name) <= 1000:
            parser.error(f"--{name.replace('_', '-')} must be 0..1000")
    if not 0 <= args.heat_us <= 2500:
        parser.error("--heat-us must be 0..2500 (the configured per-slice ceiling)")
    if not 2 <= args.event_ms <= 65:
        parser.error("--event-ms must be 2..65 (TBP ROW period is a u16 in microseconds)")
    if not 1 <= args.steps_per_event <= 8:
        parser.error("--steps-per-event must be 1..8")
    return args


def preview():
    lines = []
    for by in range(BOXES):
        lines.append(
            "".join("##" if (bx + by) % 2 == 0 else ".." for bx in range(BOXES))
        )
    return "\n".join(lines)


async def profile(con, args, count=20):
    """Time each command alone. Nothing is heated (blank data, 0 us pulse) and the motor does not move."""
    import time

    blank = b"\x00" * 48
    probes = [
        ("PSTEPSTAT (bare round trip)", dict(action="raw", command="PSTEPSTAT")),
        (
            "shift 384 bits",
            dict(
                action="printer",
                confirm="printer",
                op="shift",
                data_hex=blank.hex(),
                bit_delay_us=args.bit_us,
                data_hold_us=args.hold_us,
            ),
        ),
        (
            "latch",
            dict(
                action="printer", confirm="printer", op="latch", pulse_us=args.latch_us
            ),
        ),
        (
            "heat 0 us",
            dict(action="printer", confirm="printer", op="heat", pulse_us=0),
        ),
        ("PSTEP count 0 (no motion)", dict(action="raw", command="PSTEP FWD 0 0 1000")),
    ]
    print(f"profile: {count} runs per command")
    for name, msg in probes:
        times = []
        for _ in range(count):
            t = time.perf_counter()
            await con.call(**msg)
            times.append((time.perf_counter() - t) * 1000)
        times.sort()
        print(
            f"  {name:30s} min {times[0]:7.1f}  median {times[len(times) // 2]:7.1f}  max {times[-1]:7.1f} ms"
        )

    # Delay-loop calibration: the firmware busy-wait is "us * 12 NOPs" and was never
    # calibrated. Blank data + latch pulse only, so nothing is heated.
    async def med(msg, n=5):
        ts = []
        for _ in range(n):
            t0 = time.perf_counter()
            await con.call(**msg)
            ts.append((time.perf_counter() - t0) * 1000)
        return sorted(ts)[n // 2]

    base = await med(dict(action="printer", confirm="printer", op="latch", pulse_us=0))
    long = await med(
        dict(action="printer", confirm="printer", op="latch", pulse_us=1000)
    )
    print(f"  latch 0 us / 1000 us: {base:.1f} / {long:.1f} ms")
    factor = (long - base) / 1.0
    print(
        f"  => firmware 'us' delays run about {factor:.1f}x longer than nominal "
        f"(1.0 = calibrated; a requested 2000 us heat pulse would last about {2.0 * factor:.1f} ms)"
    )
    zero = await med(
        dict(
            action="printer",
            confirm="printer",
            op="shift",
            data_hex=blank.hex(),
            bit_delay_us=0,
            data_hold_us=0,
        )
    )
    print(f"  shift with 0 us delays (pure GPIO/loop overhead): {zero:.1f} ms")
    t = time.perf_counter()
    await con.batch([m for _, m in probes[1:]])
    print(
        f"  4 commands pipelined in one batch: {(time.perf_counter() - t) * 1000:.1f} ms"
    )


async def run(args):
    total_events = ROW_COUNT * EVENTS_PER_ROW
    async with aiohttp.ClientSession(
        timeout=aiohttp.ClientTimeout(total=None)
    ) as session:
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
            print(
                f"plan: {BOXES}x{BOXES} checkerboard of {BOX_PX}-dot boxes = {HEAD_DOTS}x{ROW_COUNT} dots"
            )
            print(preview())
            print(
                f"{ROW_COUNT} rows x {EVENTS_PER_ROW} events = {total_events} print events "
                f"(shift+latch+{args.heat_us} us heat, {args.steps_per_event} half-step(s) each), event period {args.event_ms} ms "
                f"-> about {total_events * args.event_ms / 1000:.1f} s of nominal pacing"
            )
            print(
                f"motor: {total_events * args.steps_per_event} requested half-step transitions across the four events per logical row"
            )
            print(
                "Reminder: stock timings are static-analysis-derived; heater limits are not validated."
            )
            if args.dry_run:
                print(f"dry-run row 0: {make_row(0).hex()}")
                print(f"dry-run row {BOX_PX}: {make_row(BOX_PX).hex()}")
                print("dry run: no GPIO, printhead, heater, or step commands sent")
                return
            if args.profile:
                configured = False
                try:
                    for pin in HEAD_PINS + MOTOR_PINS:
                        await con.call(action="mode", pin=pin, mode="lo", confirm=pin)
                    configured = True
                    await con.raw("SET PB8=1 PB9=0 PB10=0 PB11=0")
                    await profile(con, args)
                finally:
                    if configured:
                        await con.raw(
                            "SET PB8=1 PB9=0 PB10=0 PB11=0 "
                            + " ".join(f"{pin}=0" for pin in MOTOR_PINS)
                        )
                return
            if not args.yes:
                print(
                    "This submits a multi-row job to firmware and may energise the heater. Type PRINT to continue: ",
                    end="",
                    flush=True,
                )
                if (await asyncio.to_thread(sys.stdin.readline)).strip() != "PRINT":
                    raise SystemExit("aborted")

            # One host request carries all stock-style row events. Firmware queue credits
            # bound the in-flight window; each command's terminal STATUS is consumed by
            # the Bun transport before the next credit is used.
            job_rows = []
            for y in range(ROW_COUNT):
                mask = make_row(y).hex()
                for _ in range(EVENTS_PER_ROW):
                    job_rows.append(
                        {
                            "maskHex": mask,
                            "burnUs": args.heat_us,
                            "steps": args.steps_per_event,
                            "periodUs": args.event_ms * 1000,
                            "slices": 1,
                        }
                    )
            print(
                f"submitting one job: {len(job_rows)} ROW commands (4 events per logical row)"
            )
            result = await con.call(action="print-job", rows=job_rows)
            if result.get("error"):
                raise RuntimeError(result["error"])
            print(
                f"terminal status: {result.get('done')}/{result.get('total')} rows; stopped={result.get('stopped', False)}"
            )
            print(
                "Note: host queueing is asynchronous, but the current firmware still serializes shift/heat/motion; row-boundary continuity is not yet hardware-validated."
            )
            print("done")


def main():
    args = parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)


if __name__ == "__main__":
    main()
