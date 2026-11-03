#!/usr/bin/env python3
"""Print a 384-dot-wide vertical band ramp: 1 white/black, 2 white/black, etc.

Requires Pinbridge WebUI at ws://127.0.0.1:8765/ws and a connected printer.
The mask assumes LSB-first data and bit 1 = black; that polarity remains an
experimental hypothesis. The image is 384 logical rows tall so the bands are
long enough to identify orientation. It submits four ROW events per logical
row, following the project's stock-scheduler model; hardware timing is not
validated. Heater safety gates remain enforced by the Bun host/firmware.
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
ROW_COUNT = 384
EVENTS_PER_ROW = 4


class Console:
    def __init__(self, ws):
        self.ws = ws

    async def batch(self, msgs):
        for msg in msgs:
            await self.ws.send_json(msg)
        replies = []
        while len(replies) < len(msgs):
            reply = await asyncio.wait_for(self.ws.receive_json(), timeout=70)
            kind = reply.get("type")
            if kind in FAIL_TYPES:
                raise RuntimeError(f"{kind}: {reply.get('message', reply)}")
            if kind in {"print-progress", "job-status"}:
                if kind == "print-progress":
                    print(f"terminal events: {reply.get('done')}/{reply.get('total')}")
                continue
            if kind in OK_TYPES:
                replies.append(reply)
        return replies

    async def call(self, **msg):
        return (await self.batch([msg]))[0]

    async def raw(self, command):
        return await self.call(action="raw", command=command)


def band_pattern(width=HEAD_DOTS):
    """Return (is_black, width) bands, clipping the final band to exact width."""
    if width <= 0:
        raise ValueError("width must be positive")
    bands = []
    used = 0
    band_width = 1
    while used < width:
        for is_black in (False, True):
            actual = min(band_width, width - used)
            bands.append((is_black, actual))
            used += actual
            if used == width:
                break
        band_width += 1
    return bands


def make_mask(width=HEAD_DOTS):
    """Build a 48-byte LSB-first row mask from the requested band sequence."""
    if width != HEAD_DOTS:
        raise ValueError("Pinbridge rows must be exactly 384 dots")
    bits = []
    for is_black, band_width in band_pattern(width):
        bits.extend([int(is_black)] * band_width)
    return bytes(
        sum(bits[base + bit] << bit for bit in range(8))
        for base in range(0, width, 8)
    )


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws")
    parser.add_argument("--port", help="serial port if WebUI is not connected yet")
    parser.add_argument("--heat-us", type=int, default=2000,
                        help="requested burn per ROW event (firmware enforces the limits)")
    parser.add_argument("--event-ms", type=int, default=6,
                        help="event/step period in ms (2..65)")
    parser.add_argument("--steps-per-event", type=int, default=2,
                        help="motor half-step transitions per event (1..8)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--yes", action="store_true", help="skip typed PRINT confirmation")
    args = parser.parse_args()
    if args.heat_us < 0:
        parser.error("--heat-us must be 0 or more (the firmware enforces its own limits)")
    if not 2 <= args.event_ms <= 65:
        parser.error("--event-ms must be 2..65")
    if not 1 <= args.steps_per_event <= 8:
        parser.error("--steps-per-event must be 1..8")
    return args


async def run(args):
    mask = make_mask()
    bands = band_pattern()
    print(f"pattern: {len(bands)} vertical bands across {HEAD_DOTS} dots")
    print("bands (W=white, B=black):", " ".join(f"{'B' if black else 'W'}{width}" for black, width in bands))
    print(f"image height: {ROW_COUNT} rows; {ROW_COUNT * EVENTS_PER_ROW} ROW events")
    print(f"requested heat: {args.heat_us} us/event; event period: {args.event_ms} ms")
    if args.dry_run:
        print("mask hex:", mask.hex())
        print("dry run: no WebSocket, print, or motion commands sent")
        return

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
            if not args.yes:
                print("This submits a multi-row print and may energise the heater. Type PRINT to continue: ", end="", flush=True)
                if (await asyncio.to_thread(sys.stdin.readline)).strip() != "PRINT":
                    raise SystemExit("aborted")

            job_rows = []
            for _ in range(ROW_COUNT):
                for _ in range(EVENTS_PER_ROW):
                    job_rows.append({
                        "maskHex": mask.hex(),
                        "burnUs": args.heat_us,
                        "steps": args.steps_per_event,
                        "periodUs": args.event_ms * 1000,
                        "slices": 1,
                    })
            result = await con.call(action="print-job", rows=job_rows)
            if result.get("error"):
                raise RuntimeError(result["error"])
            print(f"terminal status: {result.get('done')}/{result.get('total')} events; stopped={result.get('stopped', False)}")
            print("done; if bands appear reversed, the physical dot order is opposite the current LSB-first assumption.")


def main():
    args = parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)


if __name__ == "__main__":
    main()
