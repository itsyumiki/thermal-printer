# Sprint 5 — Stepper in the loop

**Size:** L  
**Depends on:** Sprint 4  
**Goal:** Drive the motor phases from deadline checks in the loop, and remove the SDK step timer and its race workarounds.

## Read first

- `pb_step_tick`, `pb_step_start`, `pb_step_feed_schedule*`, `step_lock` in `pb_printer.h`
- `sync_step_timer` and `pb_step_timer` in `app.c`
- `docs/bugs/2026-11-03-move-wedge-and-watchdog-cancel.md`
- `tests/c/pb_race_test.c` and the `PB_RACE_POINT` hooks

## Tasks

1. Owner decision first: the target paper speed and the minimum step period. The SDK callback is really 2 ms, so today's "1 ms" floor never existed.
2. Convert `step_wait_ms` countdowns to microsecond deadlines against `micros()`, and let `pb_poll` advance phases when due. Keep the phase-segment ring and the cumulative-target semantics.
3. Delete `sys_hi_timer` use, `sync_step_timer`, `step_timer_id`, and `step_lock`. Rework or retire the race test, and document why the class of bug is gone.
4. Update the host's speed and period math, and `PB_TUNE_STEPPER_MIN_PERIOD_US`, to the measured real period.
5. Keep the boot wiggle, coil hold and idle-release behavior unchanged and re-tested.

## Acceptance criteria

- Scope or PTIME-style measurement: step period within 2 % of the request across at least three periods.
- No lost or doubled phases over a long feed (counted from the cumulative target).
- ESTOP stops the motor within one period.
- Contract tests that named `sys_hi_timer` or the old step ISR are updated; all suites pass; CI builds.

## Out of scope

Shift speedup, head timing changes.

## Handoff on completion

Include measured periods, the new floor, and the removed code list. Next: Sprint 6.
