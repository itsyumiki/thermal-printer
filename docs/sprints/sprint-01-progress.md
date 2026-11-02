# Sprint 1 — progress checkpoint

Date: 2026-11-02  
Status: **in progress; not accepted as complete**.

## Changes in this checkpoint

- C TBP duplicate detection now uses the 1..255 sequence-cycle distance and a per-session terminal-state history, retains the last 127 accepted IDs, clears history on HELLO, and rejects stale IDs not in retained history rather than replaying an arbitrary last-command result.
- Host retry logic now resends the identical serialized request if ACK arrived but terminal STATUS did not. Previously that path set the retry count to its limit and dropped the connection without resending. A `ERR SEQ` whose expected ID equals the pending command ID also retries that exact frame within the normal retry budget; if the expected ID differs, the operation fails closed instead of silently changing a ROW/MOVE command ID.
- Expanded host framing tests to explicitly reject mixed-hex and bad-CRC frames. Firmware now reports `ERR FORMAT` for malformed TBP-looking mixed-hex lines (four or more leading hex characters) rather than passing them into the legacy parser; a C transcript guards this without regressing word-led legacy commands such as `ADC`. Embedded CR is retained for format rejection while a trailing CR before LF is stripped for CRLF support.
- Spec clarified maximum wire-line length vs C NUL storage and the 38-byte INFO body vs 39-byte enclosing STATUS DATA. Duplicate response semantics are explicit, with a reminder that asynchronous execution must preserve actual state.
- Added C transcript vectors for duplicate/future ID handling, wrap from `FF` to `01`, a direct core-history boundary test covering eviction at 127 retained IDs and wrap eviction, duplicate COILS, ROW and MOVE side-effect suppression, rejected-future-ID recovery, malformed mixed hex, embedded CR, and overlong-line resynchronization; added a Bun receive test for reserved event ID `00`.
- Extracted `PrinterLink` as an importable class without starting the server on import and added deterministic fake-serial Bun tests for lost ACK, lost terminal STATUS, retry exhaustion/link-down, disconnect clearing pending work, and HELLO resetting the host ID sequence.

## Checks

- `cd host/pinbridge/bun && bun run check` — PASS, 7 tests / 24 assertions, Prettier clean.
- `cd host/pinbridge && .venv/bin/python -m unittest discover -s tests -v` — PASS, 34 tests; all C transcript fixtures including `tbp-sequence.txt`, `tbp-wrap.txt`, and `tbp-history-boundary.txt` pass under GCC `-std=c11 -Wall -Wextra -Werror`.
- `git diff --check` — PASS for this follow-up checkpoint.

## Remaining Sprint 1 work

- Host transactions are now serialized to match the current one-command printer window. On `ERR SEQ`, the host retries the rejected payload at the reported expected ID; a concurrent mismatch fails closed. Fake-transport tests cover both same-ID retry and recovery to a different expected ID.
- Add deterministic fake-transport tests for lost ACK, lost terminal STATUS, retry exhaustion, and reconnect/session reset. The terminal retry code path is fixed but not yet covered by a transport-level test.
- C transcripts assert duplicate COILS-on, heat ROW, and MOVE do not repeat hardware writes. The C harness advances the stepper tick during simulated delay, enabling deterministic motion replay checks without physical hardware.
- Overlong-line discard/resync is covered at 521 characters followed by a valid HELLO, and a dedicated 520-character valid-length frame proves the exact maximum is parsed rather than discarded.
- Resolve ESTOP ID/ACK details and make HELLO/session error handling fully consistent. Do not start executor refactoring here; that is Sprint 2.

## Evidence boundary

No firmware build was run locally, no firmware was flashed, and no hardware was exercised. This checkpoint is protocol-core and host test evidence only.
