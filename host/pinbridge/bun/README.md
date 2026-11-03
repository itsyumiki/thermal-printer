# Pinbridge Bun host

This is the production browser UI and UART host for Pinbridge. It keeps WebSocket handling, serial-port reconnect, TBP v1 framing/CRC, command IDs, and UI in one JavaScript runtime. The visual tokens and component feel are adapted from [itsyumiki/design-system](https://github.com/itsyumiki/design-system), with the missing device states and controls supplied here.

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

The Python `pinbridge webui` launcher starts this Bun server; `pinbridge webui --mock` uses the same UI and a small in-process virtual printer, so protocol/UI development stays on one host implementation.

## Protocol and controls

`tbp.ts` implements TBP v1 CRC-16/CCITT-FALSE, frame validation, signed motion, ROW payloads and INFO parsing. The firmware recognizes the same newline-delimited hex frames and implements HELLO, STAT, FLUSH, CONFIG echo, ROW, MOVE, DWELL, COILS release and ESTOP. The current MCU executor performs each row/move/dwell synchronously in the command task and reports terminal status; it is a working protocol bridge, **not yet the fully asynchronous look-ahead queue described by the TBP draft**. Do not assume continuous motion between rows or queue depth as implemented until that executor is upgraded and hardware-validated.

The power button and connection LED are local firmware behavior: hold PB1 for one second from boot to latch PA1 high; after boot, hold for one second to pull PA1 low. PC7 slowly blinks without a TBP session, flashes twice on HELLO, then remains on for the session. These behaviors have not been hardware-tested here.

## Checks

```sh
bun test
```

The GitHub Actions workflow checks the JS host and separately builds/packages firmware. No local firmware build is part of the workflow instructions.
