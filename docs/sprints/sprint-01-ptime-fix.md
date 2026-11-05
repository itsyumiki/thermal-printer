# Sprint 1 — Trustworthy timer self-test

**Size:** S  
**Depends on:** nothing  
**Goal:** Fix the `PTIME` TIMER3 double count so the self-test can validate every later sprint.

## Read first

- `hal_time_test` in `firmware/apps/pinbridge/src/app.c`
- `timeRun` / `summarizeTimeRuns` in `host/pinbridge/bun/server.ts`
- `docs/timer-architecture-and-usage.md` section on TIMER3

## Tasks

1. In the TIMER3 loop, count a compare once. Either count only through `heater_isr_count` or only through the pending flag, and make the ISR-fired case unambiguous. Keep the `isr=` field.
2. Decide whether the test should leave the compare IRQ enabled. If so, record that restarting the timer per ISR adds a small bias and report it. Otherwise mask it for the run and report that separately.
3. Update the host summary so `impliedOscMHz` uses the corrected count, and keep the jiffy cross-check.
4. Add a static contract check that the TIMER3 loop cannot count one compare through both the ISR counter and the pending flag, and keep the existing bun tests for the host summary.
5. Correct `docs/timer-architecture-and-usage.md`: the TIMER3 inconsistency was a test bug, and record the new results.

## Acceptance criteria

- On hardware, all four TIMER3 rows report a ratio within 2 % and an implied oscillator near 24 MHz.
- `isr` per run matches the expected period count.
- Python/C and Bun suites pass; CI builds.

## Out of scope

New timer sources, any change to heater behavior.

## Handoff on completion

Paste the owner's new self-test table into the doc. Next: Sprint 2.
