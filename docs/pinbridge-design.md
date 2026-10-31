# pinbridge: design (protocol v2, "jlpin")

Status: **design, nothing implemented yet** (2026-11-01). Written before the firmware so the protocol is not only in a prompt.
Tags: [verified] seen in SDK headers/logs, [to verify] must be checked against the SDK before relying on it, [estimate] a guess, [open] needs an owner decision.

## 1. Goal
A Firmata-like demo for the AC6956C / AC635N (BR23) board: the chip runs a small "pin server" over USB CDC, and **all logic lives in Python on the host**, so
pin-level experiments (blink, read a button, sweep ADC, drive a stepper pattern, find the printer's pin map) need no reflashing.
The protocol is a plain-text line protocol that a human can type into a serial terminal.

Non-goals: firmware-timed waveforms (strobes, microsecond pulses), a GUI, Bluetooth transport (the protocol is line-based and could ride the BLE bridge later), driving anything safety-critical.

## 2. Relationship to gpiodemo (`~/projects/da-vinci-jr-1.0-hacking/gpiodemo`)
We borrowed ideas, not the wire format. gpiodemo targets MCUs with unknown pin connections and a high pin count, plus chained UART nodes. This chip has ~10 usable pins and one host, so we dropped:
node/route tokens, 3-digit request ids, the `<3` terminator, `MAP` discovery, banks and `ALL` targets, `IDK/UMM/HYG/OKA/HII/CYA` vocabulary.
Kept: line-based ASCII, **pins are untouched until explicitly configured**, persistent change listeners that emit events, a reset command, host-side fake for tests.
History for reference (read-only): before commit `1224fb6` (Rust rewrite) gpiodemo was a Python master + C slave; `git show 1224fb6^:gpiodemo/slave/src/protocol.c` is a ~550-line C engine (line buffer, tokenizer, UNSET/IN/OUT state, listeners, TX queue) over a tiny GPIO HAL, and `gpiodemo/master/mock_serial.py` is a fake serial. Useful as a structural reference only (v1 used numeric pin ids; we use names). Licence: that repo is AGPLv3, this one GPLv3, same owner; note provenance in any derived file. gpiodemo never had ADC.

## 3. Safety (read first)
The target is a thermal printer whose pin map is unknown ([open] docs/journal.md). Thermal-head strobes, motor coils and power-enable lines may be on any pin; raw pin control can overheat them.
- **First runs on USB power only, thermal head disconnected, motor/battery supply off.**
- **Boot:** the firmware configures no GPIO beyond what the SDK itself does. Every pin is "untouched" until the host sends `MOD`. `SET/GET/ADC/LSN` on an untouched pin fail with `err mod`; nothing is configured implicitly.
- **Pin config** (section 4) is the only place that decides which pins exist. Anything not confirmed as bonded out, free of SDK modules and safe to touch is `deny` (not in the table, answers `err pin`). USB DP/DM, pins owned by enabled SDK modules (UART0/PA0 log is off in this app; ADKEY on PB1 must be switched off in app.conf, see section 9) and anything unverified start denied.
- **Watchdog is ON by default** (5000 ms): see section 7.
- The safe state ("RST") is: input, no pulls, digital input buffer off where the SDK allows, PWM stopped, listeners cleared.

## 4. Single pin config (the "no MAP" decision)
There is one chip and one host, so there is no on-wire pin discovery. A single source file (suggested `firmware/apps/pinbridge/pins.toml`) lists, per allowed pin:
name (`PC7`), capability flags (`in out pu pd adc pwm`), optional ADC channel, optional PWM channel/note, optional host-only `alias` (`led`), and a free-text note. A generator script emits
(a) a C header with the table, (b) a Python module with the same table. Both generated files are committed; a check script regenerates and diffs them (same idea as `scripts/check-shared-core.sh`).
Capability flags are **never sent on the wire**.
**Drift guard:** the generator computes a 32-bit hash (FNV-1a over a canonical text of name+caps+channels, aliases/notes excluded) printed as 8 hex digits. `HEL` returns it; the Python `Board.open()` refuses to run if it differs from its own table.
Initial contents: the agent infers the bonded-out pins from the docs on the box; PC7 (blink/heartbeat pin from hello-cdc) is the known-good test pin; everything it cannot confirm stays denied with a note why. [open] which pins the owner actually wants.

## 5. Wire format
- ASCII lines ending `\n`; `\r` is ignored; empty lines and lines starting with `#` from the host are ignored (no reply). Max **128 bytes** per line each way. Longer host lines give `err len` and resync at the next `\n`.
- Commands are 3 letters, case-insensitive, shown upper-case. Pins are native names `P<port><n>` (`PA0`..`PD7`, no zero padding), case-insensitive on input, upper-case in replies. Modes are lower-case.
- Strict stop-and-wait: **one command, one reply line, in order.** Firmware also processes pipelined lines in order, but hosts should wait for each reply.
- Replies: `ok [data...]` or `err <code> [text]`. Unsolicited lines: `!<pin>=<0|1> @<ms>` (listener event), `!wdg` (watchdog tripped), `#...` (debug, only after `DBG on`). A reply or event line is always written whole; they never interleave inside a line.
- Parsers must ignore unknown trailing `key=value` tokens in replies (room to grow). Nothing else is ever printed on the port: SDK/BT logs are off in this app.
- Error codes (3 letters): `cmd` unknown command, `arg` malformed/missing argument, `pin` pin not in the table, `cap` pin lacks that capability, `mod` wrong or missing mode for this operation, `len` line too long, `bsy` TX queue full / resource busy.
- **Multi-pin commands are all-or-nothing:** every argument is validated first; on any error nothing is applied and the error names the first offending token.

## 6. Commands
| cmd | form | reply | notes |
|---|---|---|---|
| `HEL` | `HEL` | `ok jlpin 2 <cfg8> up=<ms>` | hello, keepalive and reset detection in one. `up` going backwards means the board rebooted. |
| `MOD` | `MOD <pin> <mode>` | `ok` | atomic mode change, no glitch on entry |
| `SET` | `SET <pin>=<0\|1> ...` | `ok` | pins must be in an output mode; one command = atomic per port |
| `GET` | `GET [<pin> ...]` | `ok PC0=1 PC1=0` | no args = every pin currently in a digital mode |
| `ADC` | `ADC <pin\|vbat> ...` | `ok PB1=412/1219` | `<raw>/<mV>`; pin must be in `adc` mode; `vbat` needs no mode |
| `PWM` | `PWM <pin> <permille> [hz]` | `ok` | optional (section 10); pin must be in `pwm` mode; permille 0..1000 |
| `LSN` | `LSN <pin> ...` / `LSN off [<pin> ...]` | `ok` | digital input pins only; events `!PB3=0 @48977`; `off` with no pins clears all |
| `STA` | `STA [<pin> ...]` | `ok PC7=hi PB3=up PB1=adc PC0=off` | modes (`off` = untouched); output pins show their current level (`lo`/`hi`) |
| `WDG` | `WDG <ms>` | `ok` | 0 = off; default 5000 |
| `RST` | `RST` | `ok` | all pins to the safe state, listeners cleared, PWM stopped |
| `DBG` | `DBG on\|off` | `ok` | enables `#` debug lines |

**Modes** for `MOD`: `hiz` (safe state: input, no pull, digital buffer off), `in` (digital input, no pull), `up` (input + pull-up), `dn` (input + pull-down), `lo` / `hi` (output, initial level set atomically), `adc` (analog: digital buffer off, pulls off), `pwm`. A mode the pin's flags don't allow gives `err cap`.
`GET` on an output pin returns the pad level if the SDK allows reading it back, else the last written level [to verify].
`SET` with several pins is atomic per port; the SDK's `gpio_out(gpio, start, len, dat)` looks like the multi-bit primitive [to verify its semantics and whether pins across ports can be done in one go; if not, document the per-port granularity].
Example:
```text
HEL            ->  ok jlpin 2 7c3a91e0 up=48213
MOD PC7 lo     ->  ok
SET PC7=1      ->  ok
MOD PB3 up     ->  ok
GET PB3        ->  ok PB3=1
LSN PB3        ->  ok
               ->  !PB3=0 @48977
MOD PB1 adc    ->  ok
ADC PB1        ->  ok PB1=412/1219
SET PC0=1 PC1=0 PC2=1 PC3=0   ->  ok
RST            ->  ok
```

## 7. Watchdog and reset detection
Any valid line (including `HEL`) feeds the watchdog. If it expires while any pin is an output or PWM, firmware performs `RST` and emits `!wdg`. The Python library arms nothing extra (it is already on) and sends `HEL` at about a third of the timeout.
Also go to the safe state when the host closes the port, if the CDC line state (DTR) or a USB detach/suspend event is readable [to verify; if not, say so in the README].
Reset detection is host-side via `up=`. One unexplained board reset was seen in the BLE bridge hardware run (docs/uart-bridge-comparison.md); the firmware main loop must stay simple and any SDK watchdog/stack concern should be noted.

## 8. ADC
- ADC-capable pins from `adc_api.h`: PA1, PA5, PA6, PA10, PA12, PB1, PB3, PB4, PB8, PB10, PC4, PC5, PC6 (+ DP, internal channels like VBG) [verified in the header, bonding per package to verify].
- Report raw always; convert to mV with the SDK's own calibrated conversion (it samples the bandgap) rather than assuming a reference [to verify how vbat/vbg are converted in the SDK; the boot log shows `vbg_adc_value = 406`, which suggests roughly 3 V full scale on a 10-bit ADC if the bandgap is ~1.2 V: a sanity check only [estimate]].
- Single-shot reads; document the sampling time and any SDK averaging. Do not leave the SDK sampling service in a state that disturbs CDC timing.
- Input must stay within VDDIO [assumed, check datasheet].
- Pin-map use: a Python tool sweeps ADC pins and fingerprints nets (stable mid-scale = thermistor/divider candidate; follows the pull = floating).

## 9. Firmware architecture
- Base: `firmware/apps/hello-cdc` (CDC only, BLE off, idle app, 512K flash). Extra `app.conf` line: `TCFG_ADKEY_ENABLE DISABLE_THIS_MOUDLE` so PB1 is free and not ADC-sampled (the ADKEY floating-pin problem from the BLE run) [verify exactly one define matches]. SDK UART0/PA0 log stays off.
- **Portable core** (parser, state machine, listeners, watchdog logic) behind a small HAL struct (`mode/write/read/pull`, `adc_read`, `millis`, `tx_write`) so the same source compiles with gcc on the dev box for unit tests. New `.c` files cannot be added to the SDK Makefile list, so ship the core as headers copied by `app.conf copy` lines (new headers are allowed; as `uart_bridge.h` does) and `#include` them from the single `src/app.c` that replaces the SDK's idle app.
- **CDC:** reuse the proven pattern from `firmware/shared/uart-bridge/uart_bridge.c`: wakeup handler -> semaphore -> task -> RX ring, and writes from a task, never from the interrupt. A stalled host must not wedge command processing (drop and count replies after a timeout).
- **Timing:** the OS tick is 10 ms, so the watchdog and listener polling run at that granularity [verified in the uart-bridge docs]; look for a faster SDK timer only if it can be verified. No debounce. Events are bank-snapshot based (one port read per poll, round-robin) like gpiodemo's listener.
- Realistic round-trip over USB CDC is about a millisecond or worse [estimate]: fine for poking pins, not for kHz stepping or microsecond strobes.

## 10. PWM (optional, last)
`MOD <pin> pwm` + `PWM <pin> <permille> [hz]` via `mcpwm.h` or `pwm_led.h` (both exist in the SDK headers [verified]). Which pins/channels can do it, frequency range and resolution are [to verify]; if hardware PWM is awkward, skip it and document why. The PWM capability flag in the pin config says which pins qualify.

## 11. Host library (Python)
Match the repo's Python conventions (look for pyproject/uv/pytest and how `br23t` and its fake `Transporter` tests are organised).
- `Board(port=None)`: auto-detect the CDC port by VID/PID if feasible; context manager sends `RST` on exit; `open()` verifies protocol version and config hash (refuses on mismatch); reader thread separates `!` events from replies; keepalive `HEL`; reset detection via `up=`; timeouts everywhere (default 500 ms); `err` codes map to exceptions.
- API mirrors the commands: `mod/set/get/adc/pwm/listen/sta/wdg/rst`, pin aliases from the config, `on_event` callback or queue.
- CLI: `python -m pinbridge repl` types raw protocol lines.
- Examples: blink PC7; read a pulled-up input and print changes via `LSN`; ADC sweep + fingerprint; jumper loopback self-test; atomic 4-pin pattern writer (pins as arguments, never hardcoded to printer pins).

## 12. Testing
- **Golden transcripts**: text files of `> command` / `< reply` lines (including expected errors, events and the watchdog trip) used by BOTH the C unit tests (core + fake HAL, compiled with gcc) and the Python tests (against a Python fake and against the compiled core over a pty). One truth for the protocol.
- Cover: every command's OK path, every error code, all-or-nothing validation, line-too-long resync, `#`/blank/CRLF handling, listener lifecycle (add, `LSN off`, `RST`), ADC (fake raw values, `adc` mode rules), watchdog trip, hash mismatch, reset detection.
- Do not run cargo on the build box (AGENTS.md); pytest is fine, run niced.
- Firmware: build with `~/projects/scratch-bridge/tools/mk2.sh`; record `size` and 512K fit versus hello-cdc. No hardware on the build box: everything not run on the board is tagged [untested on hardware].

## 13. Open decisions for the owner
1. Which pins go into the config (default: only confirmed ones, rest denied).
2. Watchdog default 5000 ms: keep, or longer?
3. PWM: worth it if only a few pins support it?
4. Later: carry the same protocol over the BLE bridge (line-based, 128 B max, chunked)?

## 14. Changelog / deviations
(Implementers: record any deviation from this document here and in the final report, with the reason.)
