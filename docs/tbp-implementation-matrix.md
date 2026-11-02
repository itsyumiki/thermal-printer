# TBP v1 draft — implementation matrix

Snapshot: 2026-11-02, branch `pinbridge`, HEAD `f38307d00e5bff43bc37c928da53765cda13cf85`. This is a source/test inventory, not a firmware release claim. “C” means the portable C protocol path in `firmware/apps/pinbridge/src/pb_core.h`; it does not mean built, flashed, or verified on hardware.

Legend: **Implemented (partial)** means a path exists but does not meet the full spec contract; **Missing** means no corresponding protocol behavior exists; **Ambiguous** means the draft itself needs a decision before conformance can be tested.

| Spec area | Bun host | C firmware path | Gap / owner sprint |
|---|---|---|---|
| §2–3 framing, hex line, CRC-16/CCITT-FALSE | Encode/decode, CRC and malformed-frame tests | Hex parsing, length and CRC checks; HELLO transcript exercises a valid frame | Error precedence and exact FORMAT/CRC response contracts differ from draft; Sprint 1 |
| §3.1 receive validation order | Decoder rejects malformed frames; transport maps errors | Checks format/length/CRC, then ESTOP, HELLO/session, sequence and opcode payload | C does not implement every specified check/order (e.g. STOPPED); Sprint 1 |
| §4 IDs and wrap | IDs increment and skip zero; retry logic in host transport | Strict next ID, but duplicate handling only remembers the last accepted ID/state | No 127-entry duplicate result history; stale duplicates can be misclassified; Sprint 1 |
| §5 ACK/STATUS/ERR/EVENT | ACK/status handling and status parsing | ACK/STATUS/ERR emitters exist; no unsolicited event stream | Event/terminal result semantics incomplete; Sprint 1 |
| §6 states, queue, credits | Models one outstanding transaction at a time | No command queue; ROW/MOVE/DWELL execute synchronously in parser | Advertised queue depth is one; parser blocks; FLUSH/ESTOP cannot interrupt a running synchronous operation; Sprint 2 |
| §6.3 ESTOP | Sends ESTOP command | Turns heater off and stops motor when parser reaches ESTOP handler | Not asynchronously serviceable during synchronous work; no demonstrated interrupt path; Sprint 2 |
| §7.1 HELLO / INFO | Sends HELLO and decodes fixed INFO layout | Returns hard-coded capability/limit bytes; sensor/capability values are unavailable placeholders | Reported values are not measured; capabilities and build/UID fields need truthful definitions; Sprint 3 |
| §7.2 FLUSH | Sends FLUSH | Stops motor and turns heater off, replies DONE | Does not report each cancelled command/reason; no queued work to discard; Sprint 2 |
| §7.3 STAT | Parses STAT body | Returns 13-byte body; temperature, battery and paper are mostly unknown | State/flags do not represent a real executor or sensor reads; Sprint 3 |
| §7.4 CONFIG | Encodes command | Accepts key/value and echoes effective value | Values are not consistently validated or applied (notably heartbeat/idle hold/link timeout); Sprint 1 for contract, Sprint 3 for safety-related effects |
| §7.5 ROW | Builds 54-byte payload and validates common bounds | Accepts 54-byte payload and performs shift/heat/motion inline | Synchronous execution; no real scheduler, pre-shift/look-ahead, calibrated timer semantics, duty/temperature/battery/paper gates; DONE telemetry is placeholder; Sprint 2–4 |
| §7.6 MOVE | Encodes signed i32 steps and u16 period | Performs motion inline | Blocking; implementation caps steps at ±10000, rounds timing, no progress reports; Sprint 2–4 |
| §7.7 DWELL | Encodes dwell payload | Busy-waits inline with heater off | Blocks parser and ESTOP servicing; Sprint 2 |
| §7.8 COILS | No prominent host API path found in initial inventory | Energises/releases motor outputs when no step is active | IDLE/queue semantics are not fully represented; Sprint 2 |
| §7.9 opcode FF | Can send ESTOP | Immediate handler at parser dispatch | “Immediate” is not true while synchronous command loops occupy the parser; Sprint 2 |
| §8 row timing / continuous motion | Payload helpers only; no timing scheduler | Per-command shifting, heating and stepping are sequential | No independent step/heater timers or boundary continuity; Sprint 4 |
| §9 safety gates and duty budget | Host-side values are not safety authority | Hard-coded burn cap exists; no verified sensor gates/duty budget | Must not claim hardware safety until limits/sensors are established; Sprint 3 |
| §10 heartbeat/events and session timeout | Periodic STAT polling / reconnect at host | No heartbeat/event scheduler or link timeout enforcement | Sprint 1–3, according to ownership in sprint index |
| §11 host image planning | ROW helper only; no complete raster/gray scheduler identified in TBP host | No image planning | Host scheduler and queue model remain future work; Sprint 4–5 |
| §12 disconnect/session-loss safety | UART reconnect and session HELLO on host | No demonstrated asynchronous disconnect-triggered cancellation while blocked | Requires transport/firmware coordination; Sprint 1–3 |
| §13 error codes / terminal history | Parses response kinds but full conformance not established | Basic ERR codes; only minimal last-command tracking | Error-code payloads and terminal replay need conformance tests; Sprint 1 |
| §14 unknowns / physical assumptions | UI can display reported fields | Several reported limits are compile-time constants, not measured | Physical dot order, pin map, timing, thermal/electrical limits, sensors, and motor direction remain unverified; Sprint 3, 4, 6 |

## Known draft ambiguities to resolve before claiming conformance

1. **INFO length wording:** §7.1 calls INFO “38 bytes”; the STATUS response adds its state byte, so the full STATUS DATA is 39 bytes. Host `decodeInfo` currently expects 39 bytes including state. State this distinction consistently.
2. **Frame maximum:** §3 says max line length is 521 including LF, while a 255-byte payload gives 520 hex characters plus LF = 521; the C line buffer comment reserves 523 bytes and Bun rejects hex strings longer than 520 excluding newline. Document buffer size separately from wire line length.
3. **Duplicate replay:** §4 asks for the last 127 IDs but only requires replaying a recorded latest STATUS “state only, no payload”; clarify whether payload replay is intentionally omitted and define what to do for a duplicate older than retained history.
4. **ROW timing/result:** §7.5 defines `steps` as i8 and DONE telemetry fields; §8 requires continuous step timing, independent heater timer and look-ahead. Current C performs heat before stepping and reports placeholders/nominal values. Sprint 4 must define measurable acceptance without pretending current behavior conforms.
5. **Safety constants:** §9 marks key thermal/duty/guard values TBD. Until owner-approved values and real sensor acquisition exist, no heater-enabled physical test should be treated as safe validation.
6. **ESTOP semantics:** §3.1 says ESTOP skips session and sequence checks; §6.3 requires heater off immediately. A synchronous parser cannot meet this while blocked in ROW/MOVE/DWELL. Sprint 2 must choose and implement an interruptible execution architecture.
7. **CONFIG defaults/effects:** the spec lists heartbeat, idle-hold and link-timeout settings, but current C mostly echoes values. Specify which settings must persist for a session, and test effective behavior rather than echo-only replies.

## Test evidence boundaries

- `host/pinbridge/bun/tbp.test.ts`: CRC check vector, HELLO frame vector, ROW payload round-trip/field offsets, corrupt CRC/length rejection, signed MOVE payload, INFO decoding.
- `firmware/apps/pinbridge/tests/transcripts/tbp-hello.txt`: HELLO request/ACK/INFO transcript through the C runner.
- The shared C/Python transcript suite is primarily the legacy text command interface; it is **not** evidence of full TBP v1 conformance.
- See [`sprint-00-report.md`](sprints/sprint-00-report.md) for exact baseline commands and results.
