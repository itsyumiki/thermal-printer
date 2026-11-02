# Sprint 1 — TBP framing, sessions and recovery

**Size:** S  
**Depends on:** Sprint 0 baseline  
**Goal:** Make the transport/session state machine match the agreed wire specification before adding concurrency.

## Read first

- `docs/thermal-protocol-spec.md` sections 3–5, 7.1, 12–14
- `host/pinbridge/bun/tbp.ts` and `tbp.test.ts`
- Firmware frame parser and command-ID logic in `firmware/apps/pinbridge/src/pb_core.h`
- Sprint 0 implementation matrix

## Tasks

1. Resolve the current draft-vs-code differences: frame minimum/maximum length, ID 00 rules, HELLO validation, sequence wrap, and how ESTOP response IDs are handled. Update the spec before coding ambiguous behavior.
2. Make host and C parser behavior agree on malformed hex, odd lengths, bad LEN, CRC failures, CRLF, empty lines, and overlong-line discard/resync.
3. Implement strict sequential IDs with wrap from FF to 01, identical-frame retries, and deterministic duplicate handling. A duplicate must never execute the command twice; replay the recorded status/result required by the final spec, not a guessed generic state.
4. Implement session reset behavior consistently on HELLO, UART disconnect/reconnect, MCU reset, and link timeout. A new session must clear stale pending work on both sides.
5. Ensure a rejected command does not consume its ID; host stops sending later commands and retries from the expected ID on ERR SEQ.
6. Add tests for lost ACK, lost STATUS, duplicate frame, future ID, wraparound, reconnect, and bad CRC. Use a fake transport; do not require physical UART.

## Acceptance criteria

- Host and C golden tests agree for the resolved framing/session cases.
- Duplicate ROW/MOVE test proves exactly-once execution semantics in the model/transcript.
- Reconnect cannot accidentally continue an old session or issue stale queued commands.
- Retry limits and transition to link-down are deterministic and observable.
- Bun tests, C transcript suite, formatting, and diff checks pass.

## Out of scope

Nonblocking motion execution, new safety thresholds, and UI visual changes beyond surfacing reliable session state.

## Handoff on completion

Record any spec changes, golden vectors added, retry timing assumptions, and test results. Next: Sprint 2 executor; it must use the finalized ACK/STATUS/ERR/session contract.
