# Sprint 1 — progress checkpoint

Date: 2026-11-02  
Status: **in progress; not accepted as complete**.

## Changes in this checkpoint

- C TBP duplicate detection now uses the 1..255 sequence-cycle distance and a per-session terminal-state history, retains the last 127 accepted IDs, clears history on HELLO, and rejects stale IDs not in retained history rather than replaying an arbitrary last-command result.
- Host retry logic now resends the identical serialized request if ACK arrived but terminal STATUS did not. Previously that path set the retry count to its limit and dropped the connection without resending. A `ERR SEQ` whose expected ID equals the pending command ID also retries that exact frame within the normal retry budget; if the expected ID differs, the operation fails closed instead of silently changing a ROW/MOVE command ID.
- Expanded host framing tests to explicitly reject mixed-hex and bad-CRC frames. Firmware now reports `ERR FORMAT` for malformed TBP-looking mixed-hex lines (four or more leading hex characters) rather than passing them into the legacy parser; a C transcript guards this without regressing word-led legacy commands such as `ADC`.
- Spec clarified maximum wire-line length vs C NUL storage and the 38-byte INFO body vs 39-byte enclosing STATUS DATA. Duplicate response semantics are explicit, with a reminder that asynchronous execution must preserve actual state.
- Added C transcript vectors for duplicate/future ID handling, wrap from `FF` to `01`, and a direct core-history boundary test covering eviction at 127 retained IDs and wrap eviction; added a Bun receive test for reserved event ID `00`.

## Checks

- `cd host/pinbridge/bun && bun run check` — PASS, 7 tests / 24 assertions, Prettier clean.
- `cd host/pinbridge && .venv/bin/python -m unittest discover -s tests -v` — PASS, 34 tests; all C transcript fixtures including `tbp-sequence.txt`, `tbp-wrap.txt`, and `tbp-history-boundary.txt` pass under GCC `-std=c11 -Wall -Wextra -Werror`.
- `git diff --check` — PASS for this follow-up checkpoint.

## Remaining Sprint 1 work

- Host ERR SEQ recovery is deliberately conservative when the printer reports an expected ID different from the current command. The current command API serializes transactions, so there is no durable multi-frame in-flight queue to replay; safe recovery across an ID gap requires explicit operation semantics rather than blindly renumbering potentially non-idempotent ROW/MOVE commands.
- Add deterministic fake-transport tests for lost ACK, lost terminal STATUS, retry exhaustion, and reconnect/session reset. The terminal retry code path is fixed but not yet covered by a transport-level test.
- Duplicate ROW/MOVE no-reexecution still needs a dedicated side-effect assertion beyond the state-only protocol transcript.
- Continue aligning CRLF and overlong-line behavior between the C receiver and host parser without breaking the legacy text-command interface; embedded CR is currently ignored by the firmware receiver, while the spec only permits CR immediately before LF.
- Resolve ESTOP ID/ACK details and make HELLO/session error handling fully consistent. Do not start executor refactoring here; that is Sprint 2.

## Evidence boundary

No firmware build was run locally, no firmware was flashed, and no hardware was exercised. This checkpoint is protocol-core and host test evidence only.
