# TBP v1 draft — implementation matrix

Snapshot: 2026-11-02, refreshed after Sprint 1 completion on branch `pinbridge`. This is a source/test inventory, not a firmware release claim. “C” means the portable C protocol path in `firmware/apps/pinbridge/src/pb_core.h`; it does not mean built, flashed, or verified on hardware.

Legend: **Implemented (partial)** means a path exists but does not meet the full spec contract; **Missing** means no corresponding protocol behavior exists; **Ambiguous** means the draft itself needs a decision before conformance can be tested.

| Spec area | Bun host | C firmware path | Gap / owner sprint |
|---|---|---|---|
| §2–3 framing, hex line, CRC-16/CCITT-FALSE | Encode/decode, CRC and malformed-frame tests; max 520-character wire vector | Hex parsing, length and CRC checks; malformed mixed hex, CRLF, embedded CR, max length and overlong resync have C vectors | Core framing/session acceptance covered in Sprint 1; exact wire/error parity is tested, not physical UART behavior |
| §3.1 receive validation order | Decoder rejects malformed frames; transport maps errors | Format/length/CRC/session/sequence/opcode validation has transcript coverage; STOPPED and interruptible ESTOP remain executor issues | Core Sprint 1 validation order is covered; remaining STOPPED behavior belongs to Sprint 2 |
| §4 IDs and wrap | Serialized IDs, identical-frame retry, expected-ID recovery on ERR SEQ | Strict next ID, 1..255 wrap, 127-entry per-session terminal-state history, stale-ID rejection | Sprint 1 protocol reliability acceptance complete; async history must store actual states in Sprint 2 |
| §5 ACK/STATUS/ERR/EVENT | ACK/status handling, serialized transactions, bounded retries and disconnect cleanup | ACK/STATUS/ERR emitters and state-only duplicate replay | Synchronous terminal replay covered; progress/events and actual QUEUED/RUNNING states remain Sprint 2 |
| §6 states, queue, credits | Enforces one outstanding transaction at a time | No command queue; ROW/MOVE/DWELL execute synchronously in parser | Advertised queue depth is one; parser blocks; FLUSH/ESTOP cannot interrupt a running synchronous operation; Sprint 2 |
| §6.3 ESTOP | Sends ESTOP command | Turns heater off and stops motor when parser reaches ESTOP handler | Not asynchronously serviceable during synchronous work; no demonstrated interrupt path; Sprint 2 |
| §7.1 HELLO / INFO | Sends HELLO and decodes fixed INFO layout | Returns hard-coded capability/limit bytes; sensor/capability values are unavailable placeholders | Reported values are not measured; capabilities and build/UID fields need truthful definitions; Sprint 3 |
| §7.2 FLUSH | Sends FLUSH | Stops motor and turns heater off, replies DONE | Does not report each cancelled command/reason; no queued work to discard; Sprint 2 |
| §7.3 STAT | Parses STAT body | Returns 13-byte body; temperature, battery and paper are mostly unknown | State/flags do not represent a real executor or sensor reads; Sprint 3 |
| §7.4 CONFIG | Encodes command | Validates supported key/value bounds and echoes effective value | Heartbeat/idle-hold/link-timeout values are not consistently applied; Sprint 3 for behavior and safety effects |
| §7.5 ROW | Builds 54-byte payload and validates common bounds | Accepts 54-byte payload and performs shift/heat/motion inline | Synchronous execution; no real scheduler, pre-shift/look-ahead, calibrated timer semantics, duty/temperature/battery/paper gates; DONE telemetry is placeholder; Sprint 2–4 |
| §7.6 MOVE | Encodes signed i32 steps and u16 period | Performs motion inline | Blocking; implementation caps steps at ±10000, rounds timing, no progress reports; Sprint 2–4 |
| §7.7 DWELL | Encodes dwell payload | Busy-waits inline with heater off | Blocks parser and ESTOP servicing; Sprint 2 |
| §7.8 COILS | No prominent host API path found in initial inventory | Energises/releases motor outputs when no step is active | IDLE/queue semantics are not fully represented; Sprint 2 |
| §7.9 opcode FF | Can send ESTOP | Immediate handler at parser dispatch | “Immediate” is not true while synchronous command loops occupy the parser; Sprint 2 |
| §8 row timing / continuous motion | Payload helpers only; no timing scheduler | Per-command shifting, heating and stepping are sequential | No independent step/heater timers or boundary continuity; Sprint 4 |
| §9 safety gates and duty budget | Host-side values are not safety authority | Hard-coded burn cap exists; no verified sensor gates/duty budget | Must not claim hardware safety until limits/sensors are established; Sprint 3 |
| §10 heartbeat/events and session timeout | Periodic STAT polling / reconnect at host | No heartbeat/event scheduler or link timeout enforcement | Firmware heartbeat, events and link-timeout enforcement remain Sprint 2–3 |
| §11 host image planning | ROW helper only; no complete raster/gray scheduler identified in TBP host | No image planning | Host scheduler and queue model remain future work; Sprint 4–5 |
| §12 disconnect/session-loss safety | Disconnect clears pending and queued stale commands; reconnect starts HELLO/session | Session resets on HELLO; no asynchronous disconnect cancellation while a blocking command runs | Host stale-work recovery covered in Sprint 1; firmware disconnect cancellation belongs to Sprint 2–3 |
| §13 error codes / terminal history | Handles ERR SEQ expected-ID recovery and bounded retries | Core ERR vectors and 127-entry terminal-state history have C transcript coverage | Full asynchronous result/error semantics remain Sprint 2; hardware-level error behavior unverified |
| §14 unknowns / physical assumptions | UI can display reported fields | Several reported limits are compile-time constants, not measured | Physical dot order, pin map, timing, thermal/electrical limits, sensors, and motor direction remain unverified; Sprint 3, 4, 6 |

## Known draft ambiguities to resolve before claiming conformance

1. **Resolved — INFO length wording:** INFO body is 38 bytes; enclosing STATUS DATA includes the state byte for 39 bytes total.
2. **Resolved — frame maximum:** max wire line is 520 hex characters plus LF; C allocates extra NUL storage. C transcript covers the exact 520-character frame and overlong discard/resync.
3. **Resolved — duplicate replay:** a duplicate receives ACK plus state-only STATUS; an ID outside the retained 127-ID history receives ERR SEQ and never re-executes.
4. **ROW timing/result:** §7.5 defines `steps` as i8 and DONE telemetry fields; §8 requires continuous step timing, independent heater timer and look-ahead. Current C performs heat before stepping and reports placeholders/nominal values. Sprint 4 must define measurable acceptance without pretending current behavior conforms.
5. **Safety constants:** §9 marks key thermal/duty/guard values TBD. Until owner-approved values and real sensor acquisition exist, no heater-enabled physical test should be treated as safe validation.
4. **ESTOP semantics:** §3.1 says ESTOP skips session and sequence checks; §6.3 requires heater off immediately. A synchronous parser cannot meet this while blocked in ROW/MOVE/DWELL. Sprint 2 must implement an interruptible execution architecture.
5. **CONFIG defaults/effects:** the spec lists heartbeat, idle-hold and link-timeout settings, but current C does not consistently apply them. Specify session persistence and test effective behavior rather than echo-only replies.

## Test evidence boundaries

- `host/pinbridge/bun/tbp.test.ts` and `server.test.ts`: CRC/framing vectors, malformed frame rejection, signed MOVE payload, INFO decoding, lost ACK/STATUS retries, retry exhaustion, sequence resync, serialized commands, disconnect cleanup and HELLO ID reset.
- `firmware/apps/pinbridge/tests/transcripts/`: shared C golden transcripts cover HELLO, sequence/future-ID recovery, wrap, duplicate ROW/MOVE/COILS side effects, history eviction, malformed hex, embedded CR, max-length and overlong-line resynchronization.
- These tests establish Sprint 1 protocol reliability only; they are **not** evidence of full TBP v1 conformance or hardware behavior.
- See [`sprint-00-report.md`](sprints/sprint-00-report.md) for exact baseline commands and results.
