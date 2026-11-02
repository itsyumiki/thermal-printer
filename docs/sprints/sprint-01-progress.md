# Sprint 1 — progress checkpoint

Date: 2026-11-02  
Status: **in progress; not accepted as complete**.

## Changes in this checkpoint

- C TBP duplicate detection now uses the 1..255 sequence-cycle distance and a per-session terminal-state history, retains the last 127 accepted IDs, clears history on HELLO, and rejects stale IDs not in retained history rather than replaying an arbitrary last-command result.
- Host retry logic now resends the identical serialized request if ACK arrived but terminal STATUS did not. Previously that path set the retry count to its limit and dropped the connection without resending.
- Spec clarified maximum wire-line length vs C NUL storage and the 38-byte INFO body vs 39-byte enclosing STATUS DATA. Duplicate response semantics are explicit, with a reminder that asynchronous execution must preserve actual state.
- Added C transcript vectors for duplicate/future ID handling and wrap from `FF` to `01`; added a Bun receive test for reserved event ID `00`.

## Checks

- `cd host/pinbridge/bun && bun run check` — PASS, 7 tests / 22 assertions, Prettier clean.
- `cd host/pinbridge && .venv/bin/python -m unittest discover -s tests -v` — PASS, 34 tests; all C transcript fixtures including `tbp-sequence.txt` and `tbp-wrap.txt` pass under GCC `-std=c11 -Wall -Wextra -Werror`.
- `git diff --check` — pending final commit validation.

## Remaining Sprint 1 work

- Host ERR SEQ recovery currently updates `nextId` but rejects the current command; it does not maintain/replay a durable ordered set of unfinished frames from the printer's expected ID.
- Add deterministic fake-transport tests for lost ACK, lost terminal STATUS, retry exhaustion, and reconnect/session reset. The terminal retry code path is fixed but not yet covered by a transport-level test.
- Test the history retention boundary (exactly 127 IDs vs evicted IDs) and a duplicate ROW/MOVE command's no-reexecution property beyond the state-only protocol transcript.
- Align malformed mixed-hex input, CRLF and overlong-line behavior between the C receiver and host parser without breaking the legacy text-command interface.
- Resolve ESTOP ID/ACK details and make HELLO/session error handling fully consistent. Do not start executor refactoring here; that is Sprint 2.

## Evidence boundary

No firmware build was run locally, no firmware was flashed, and no hardware was exercised. This checkpoint is protocol-core and host test evidence only.
