#!/usr/bin/env python3
"""Print a 16x16 black/white checkerboard of 24 px boxes (384x384 dots).

Requires Pinbridge WebUI running at ws://127.0.0.1:8765/ws and the printer
connected. Head data polarity (1=black) is still the experimental hypothesis.
PB8 is an active-low latch and must idle high while shifting.

This mimics the *stock* row scheduler as closely as a host script can:
  * one logical row = 4 print events with the same 48 bytes each
    (shift -> latch -> ~2 ms heater pulse), then --steps-per-event motor
    half-steps (default 2) issued as one short PSTEP;
  * events are paced at the stock minimum event period of 6 ms
    (2 ms timer tick * divider 3).

Requires pinbridge firmware where PSTEP keeps the half-step phase in firmware
RAM and resumes from it (step_phase). On older firmware every PSTEP restarts at
phase 0, so short steps would only toggle between the first phases and the paper
would not advance properly.

Limits (read these):
  * Head events are paced by the host clock, not the MCU. If the host cannot keep
    up with --event-ms, printing just runs slower (the motor still moves once per
    event, so dots stay in the right place); the worst lag is reported.
  * The step burst takes (steps-per-event + 1) ms and must finish inside one event.
  * Stock timings are static-analysis-derived, not measured. No thermal
    interlocks exist here. Not hardware-tested.
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
        help="PB11 high pulse per print event (0..6000 us; stock quality 0x32 is ~1.8 ms)",
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
    parser.add_argument(
        "--tail-steps",
        type=int,
        default=24,
        help="blank-paper feed after the pattern, in half-step transitions (default: 24)",
    )
    parser.add_argument("--tail-direction", choices=("FWD", "REV"), default="FWD")
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
    if not 0 <= args.heat_us <= 6000:
        parser.error("--heat-us must be 0..6000")
    if not 2 <= args.event_ms <= 1000:
        parser.error("--event-ms must be 2..1000")
    if args.heat_us > args.event_ms * 1000 // 2:
        parser.error(
            "--heat-us must be at most half of --event-ms (heater must not fill the whole event)"
        )
    if not 1 <= args.steps_per_event <= 8:
        parser.error("--steps-per-event must be 1..8")
    if args.steps_per_event + 1 > args.event_ms:
        parser.error(
            "step burst ((steps-per-event + 1) ms) must fit inside --event-ms"
        )
    if not 0 <= args.tail_steps <= 60000:
        parser.error("--tail-steps must be 0..60000")
    return args


def step_timing_us(event_ms):
    """Split one event period into coil-on + inter-step so one transition = event_ms (1 ms granularity)."""
    gap_ms = event_ms // 2
    return gap_ms * 1000, (event_ms - gap_ms) * 1000  # (inter_step_us, coil_on_us)


def preview():
    lines = []
    for by in range(BOXES):
        lines.append(
            "".join("##" if (bx + by) % 2 == 0 else ".." for bx in range(BOXES))
        )
    return "\n".join(lines)


async def wait_step_done(con, timeout_s=5.0):
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout_s
    while True:
        reply = (await con.raw("PSTEPSTAT")).get("reply")
        if reply and reply[0] == "done":
            return
        if loop.time() >= deadline:
            raise TimeoutError("stepper did not finish")
        await asyncio.sleep(0.002)


async def print_row(con, args, row_index, state):
    """Four print events for one logical row; each ends with one half-step transition."""
    loop = asyncio.get_running_loop()
    event_s = args.event_ms / 1000.0
    row = make_row(row_index).hex()
    for _ in range(EVENTS_PER_ROW):
        due = state["next_due"]
        now = loop.time()
        if now < due:
            await asyncio.sleep(due - now)
        else:
            state["worst_lag"] = max(state["worst_lag"], now - due)
        # Next event is due one period after this one was due (or now, if late).
        state["next_due"] = max(due, now) + event_s
        # Head work first, then the half-step, like the stock event handler.
        # PSTEP count includes the initial phase: N transitions need N+1.
        # coil_on 1 ms + gap 0: one transition per ms, the first ~1 ms after the command.
        await con.batch(
            [
                dict(
                    action="printer",
                    confirm="printer",
                    op="shift",
                    data_hex=row,
                    bit_delay_us=args.bit_us,
                    data_hold_us=args.hold_us,
                ),
                dict(
                    action="printer",
                    confirm="printer",
                    op="latch",
                    pulse_us=args.latch_us,
                ),
                dict(
                    action="printer",
                    confirm="printer",
                    op="heat",
                    pulse_us=args.heat_us,
                ),
                dict(
                    action="raw",
                    command=f"PSTEP FWD {args.steps_per_event + 1} 0 1000",
                ),
            ]
        )


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
            dict(action="printer", confirm="printer", op="latch", pulse_us=args.latch_us),
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
    print(f"  4 commands pipelined in one batch: {(time.perf_counter() - t) * 1000:.1f} ms")


async def run(args):
    inter_us, coil_us = step_timing_us(args.event_ms)
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
                f"motor: one {args.steps_per_event}-transition PSTEP per event ({total_events * args.steps_per_event} half-steps total, phase kept in firmware), "
                f"then {args.tail_steps} blank {args.tail_direction} half-steps"
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
                    "This energises the printhead heater and stepper. Type PRINT to continue: ",
                    end="",
                    flush=True,
                )
                if (await asyncio.to_thread(sys.stdin.readline)).strip() != "PRINT":
                    raise SystemExit("aborted")

            configured = False
            try:
                for pin in HEAD_PINS + MOTOR_PINS:
                    await con.call(action="mode", pin=pin, mode="lo", confirm=pin)
                configured = True
                # Latch is active-low and must idle HIGH while bits shift.
                await con.raw("SET PB8=1 PB9=0 PB10=0 PB11=0")
                state = {
                    "next_due": asyncio.get_running_loop().time(),
                    "worst_lag": 0.0,
                }
                for y in range(ROW_COUNT):
                    await print_row(con, args, y, state)
                    if (y + 1) % BOX_PX == 0:
                        print(
                            f"printed row {y + 1}/{ROW_COUNT} "
                            f"(worst host lag so far {state['worst_lag'] * 1000:.1f} ms)"
                        )
                await wait_step_done(con)
                if args.tail_steps:
                    # No shift/latch/heat: move only the paper, at the same half-step period.
                    await con.printer(
                        op="step",
                        direction=args.tail_direction,
                        count=args.tail_steps + 1,
                        inter_step_us=inter_us,
                        coil_on_us=coil_us,
                    )
                    print(
                        f"blank-paper feed complete: {args.tail_steps} half-steps (not calibrated mm)"
                    )
            finally:
                if configured:
                    try:
                        try:
                            await wait_step_done(con, timeout_s=2.0)
                        except Exception:
                            pass
                        # Heater off, latch idle high, motor phases released.
                        await asyncio.shield(
                            con.raw(
                                "SET PB8=1 PB9=0 PB10=0 PB11=0 "
                                + " ".join(f"{pin}=0" for pin in MOTOR_PINS)
                            )
                        )
                        print("heater off, motor phases released")
                    except Exception as exc:
                        print(
                            f"WARNING: unable to make safe: {exc}; use the WebUI",
                            file=sys.stderr,
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
