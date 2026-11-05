# Sprint 1 — Trustworthy timer self-test

**Status (2026-11-05):** The TIMER3 loop counts ISR deltas only, and the Bun `TIMETEST` encoder/parser/summary plus firmware adapter are included in this change. Local Bun and Python tests pass (see `docs/sprints/README.md`). CI validation is pending. Hardware acceptance remains blocked on the owner flashing a CI artifact and running the four TIMER3 cases.

**Size:** S  
**Depends on:** nothing  
**Goal:** Fix the TIMER3 double count and preserve the self-test as a binary TBP `TIMETEST` diagnostic after removal of the legacy plaintext `PTIME` command.

## Read first

- `hal_time_test` in `firmware/apps/pinbridge/src/app.c`
- `TIMETEST` command/result parser in `host/pinbridge/bun/tbp/commands.ts`, `messages.ts` and `timetest.ts`
- `docs/timer-architecture-and-usage.md` section on TIMER3

## Tasks

1. In the TIMER3 loop, count a compare once. Either count only through `heater_isr_count` or only through the pending flag, and make the ISR-fired case unambiguous. Keep the `isr=` field.
2. Decide whether the test should leave the compare IRQ enabled. If so, record that restarting the timer per ISR adds a small bias and report it. Otherwise mask it for the run and report that separately.
3. Update the host summary so `impliedOscMHz` uses the corrected count, and keep the jiffy cross-check.
4. Keep a static contract check that the TIMER3 loop cannot count one compare through both the ISR counter and the pending flag, and keep the Bun tests for the host summary and binary command framing.
5. Correct `docs/timer-architecture-and-usage.md` and `docs/thermal-protocol-spec.md`: explain the old test bug, document binary opcode `F0` `TIMETEST`, and distinguish code/test evidence from hardware results.

## Acceptance criteria

- On hardware, all four TIMER3 rows report a ratio within 2 % and an implied oscillator near 24 MHz.
- `isr` per run matches the expected period count.
- Python/C and Bun suites pass; CI builds.

## Out of scope

New timer sources, any change to heater behavior.

## Current handoff

- The firmware counts `heater_isr_count` deltas only; it does not also count TIMER3 pending-flag observations. The IRQ remains enabled and the test restarts the timer after each ISR, so a small positive timing bias is possible and has not been measured.
- The legacy text `PTIME` path was removed with the legacy protocol. This change adds diagnostic TBP opcode `F0` (`TIMETEST`) with sources `T3D4`, `T3D16`, `T3D64`, `T3D256`, `STEP`, `JIFFY`, `DELAY`, and `DELAY2`; the host times the RUNNING-to-DONE interval and reports ratio, implied oscillator, and `isr == n`.
- Local checks pass: Bun 86/86 and Python suite 52/52. This is local test evidence only. The most recent CI success predates this change, so CI validation is still required.
- Hardware table is pending. Owner: run all four TIMER3 sources at 1000 ms; require every ratio within 2 %, implied oscillator near 24 MHz, and `isr == n` (expected counts 100 / 25 / 10 / 2). No flashing was performed by this agent.

Next: after current-tree CI passes and the owner records the hardware table, close Sprint 1 and start Sprint 2. Do not begin hardware-dependent timebase conclusions from mock or CI-only evidence.
