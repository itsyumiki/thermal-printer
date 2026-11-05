# Pinbridge Bun host

This is the production browser UI and UART host for Pinbridge. It keeps WebSocket handling, serial-port reconnect, command IDs, and UI in one JavaScript runtime. TBP framing and translation live in the standalone [`tbp/`](tbp/README.md) module. The visual tokens and component feel are adapted from [itsyumiki/design-system](https://github.com/itsyumiki/design-system), with the missing device states and controls supplied here.

## Run

```sh
cd host/pinbridge/bun
bun install --frozen-lockfile
bun run start
```

Open `http://127.0.0.1:8765`. The host automatically scans serial ports and retries a disconnected UART. Optional environment settings:

- `PINBRIDGE_HOST` — bind address (default `127.0.0.1`; do not expose on a public interface without adding authentication).
- `PINBRIDGE_PORT` — HTTP/WebSocket port (default `8765`).
- `PINBRIDGE_UART` — preferred serial device path; otherwise the first detected port is selected.
- `PINBRIDGE_DEBUG` — set to `1` (or pass `--debug` to `bun run server.ts`, or `pinbridge webui --debug`) to print every raw UART frame to stdout with millisecond timestamps. Lines look like `10:51:42.123 TX    id=3d op=11 3D1106...` (`attempt N` marks retransmits), `RX    id=3d op=82 ...`, and `RX?   "!wdg" (Invalid TBP hex frame)` for anything that is not a valid TBP frame (raw text is JSON-quoted). `DROP` and `ERR` lines show why the link was torn down. Off by default; mock mode has no UART traffic to show.
- `PINBRIDGE_CONSOLE` — set to `0` (or pass `--no-console`) to turn the stdin console off, e.g. when stdin belongs to something else.

## Logs

Normal mode (no `--debug`) prints one stamped line per event that matters: UART open (with how long it took), printer connected (TBP version, build, uid, how long HELLO took), link drops and reconnects, parsed printer return codes (for example `ERR id=7 BUSY (0x09) arg=0`), successful terminal results for commands, failed or cancelled jobs, printer events (paper, over-temperature, reset), unusual ROW flags, web UI connects/disconnects, and every UI command with its result and duration. The returned code is decoded by the `tbp` parser before it is logged; raw frame hex remains behind `--debug`. Anything that can repeat every second (a missing device, a failing keepalive) is throttled to one line per 5-15 s with a count of what was held back. The 1.5 s STAT poll and heartbeats stay silent; use `--debug` to see every frame.

If the host's event loop is blocked for more than 150 ms (which is what a web UI that "waits to connect" looks like from the inside) a `host event loop was blocked for N ms during: ...` line says how long and what it was doing.

## stdin console

The UART is owned by this process, so `tio` and friends cannot open it at the same time. Type or pipe a **hex frame** into the host's stdin instead and the printer's replies are printed with a description:

```
$ echo 01010101E654 | bun run server.ts
TX 01010101E654  [hello v1]
RX 0182...  [DONE id=1 hello: TBP v1 · build ... · uid ... · 384 dots · queue 4/window 4 ...]
```

Hex only: a frame is one line of hex digits, `<ID><OP><LEN><DATA><CRC16>` (see [`docs/thermal-protocol-spec.md`](../../../docs/thermal-protocol-spec.md)); words like `stat` are refused. The frame is written exactly as typed (id and CRC untouched, so you can send deliberately broken frames), and everything the printer writes back is shown until the command's terminal reply arrives or the wait runs out. `#` comment lines and blank lines are skipped. Build frames with the `tbp` module (`Encoder`, `encodeCommand`) and decode any frame with `translate()`.

The older Python pinbridge CLI/WebUI uses the removed plaintext protocol and is incompatible with this firmware; use this Bun host only. `--mock` uses the same UI and a small in-process virtual printer, so protocol/UI development stays on one host implementation.

## Protocol and controls

[`tbp/`](tbp/README.md) implements TBP v1 CRC-16/CCITT-FALSE, frame validation, signed motion, ROW payloads, INFO parsing and the hex frame <-> command/message translator, and has no dependencies outside its folder. The firmware recognizes the same newline-delimited hex frames and implements HELLO, STAT, FLUSH, CONFIG echo, ROW, MOVE, DWELL, COILS release and ESTOP. The current MCU executor performs each row/move/dwell synchronously in the command task and reports terminal status; it is a working protocol bridge, **not yet the fully asynchronous look-ahead queue described by the TBP draft**. Do not assume continuous motion between rows or queue depth as implemented until that executor is upgraded and hardware-validated.

The power button and connection LED are local firmware behavior: hold PB1 for one second from boot to latch PA1 high; after boot, hold for one second to pull PA1 low. PC7 slowly blinks without a TBP session, flashes twice on HELLO, then remains on for the session. These behaviors have not been hardware-tested here.

## Checks

```sh
bun test
```

The GitHub Actions workflow checks the JS host and separately builds/packages firmware. No local firmware build is part of the workflow instructions.
