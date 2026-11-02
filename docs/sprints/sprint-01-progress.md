# Sprint 1 — protocol reliability completion report

Date: 2026-11-02  
Status: **complete against the Sprint 1 acceptance criteria**.

## Delivered

- C TBP duplicate detection uses the 1..255 sequence cycle and per-session terminal-state history, retains the most recent 127 accepted IDs, clears history on HELLO, and rejects stale IDs outside retained history. Duplicate HELLO/session, wraparound, future-ID rejection, and history eviction have golden coverage.
- Host transactions are serialized to match the current printer's one-command window. Lost ACK and lost terminal STATUS retry the same serialized frame within a bounded retry budget. `ERR SEQ` retries the rejected payload at the printer's reported expected ID; ambiguous concurrent mismatch fails closed. A disconnect clears pending work and queued stale commands cannot be sent into the new session.
- Firmware rejects malformed TBP-looking mixed-hex lines with `ERR FORMAT`, while word-led legacy commands such as `ADC` remain on the legacy parser path. Embedded CR is rejected; CR immediately before LF is accepted. Tests cover overlong discard/resynchronization at 521 characters and parsing of a valid 520-character maximum-size frame.
- Added C transcript side-effect assertions proving duplicate heat ROW, MOVE, and COILS-on do not repeat hardware writes. The C harness advances simulated stepper ticks during delays, allowing deterministic motion tests without physical hardware.
- Clarified the framing/session and `ERR SEQ` recovery contract in `docs/thermal-protocol-spec.md`.

## Verification

- `cd host/pinbridge/bun && bun run check` — PASS, 15 tests / 54 assertions, Prettier clean.
- `cd host/pinbridge && .venv/bin/python -m unittest discover -s tests -v` — PASS, 34 tests, including the compiled C transcript runner (`gcc -std=c11 -Wall -Wextra -Werror`).
- `./scripts/check-pinbridge.sh` — PASS, generated pin files are current.
- `git diff --check` — PASS.
- Implementation and test changes are pushed on branch `pinbridge`; this completion report is finalized in the follow-up commit.
- GitHub Actions workflow run [41501706826](https://github.com/itsyumiki/thermal-printer/actions/runs/41501706826) — PASS for firmware commit `3bc7368002483f5bf86ddc561a93c11c173029ec` (run conclusion `success`). Later documentation/test-only commits do not change the firmware sources from that build.

## Evidence boundary and Sprint 2 handoff

No firmware was flashed and no physical hardware was exercised. Sprint 1 proves host/protocol-core behavior in unit, fake-transport, and C transcript models only. Sprint 2 must preserve actual QUEUED/RUNNING/terminal states in duplicate history when asynchronous execution is introduced; the current synchronous implementation records accepted commands as terminal `DONE`. Keep the one-command host window until firmware queue semantics are implemented and advertised.
