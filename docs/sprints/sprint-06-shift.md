# Sprint 6 — Faster, bounded head shift

**Size:** M  
**Depends on:** Sprint 2 (do not land before Sprint 4)  
**Goal:** Cut the roughly 3 ms blocking shift, and make sure nothing slow runs while PB11 is high.

## Read first

- `pb_shift_head_row` in `pb_printer.h` and stage 2 of `pb_tbp_exec_poll` (the 384-bit loop)
- `write_port_fast`, `hal_delay_us` in `app.c`, and `PB_TUNE_HEAD_*` timing values
- Head timing evidence from Sprint 0

## Tasks

1. Establish the real head clock limit: from stock timing (Sprint 0) or a scope, not from the old 2 / 1 / 2 us scripts.
2. Replace per-edge `hal_delay_us` calls with direct writes plus minimal timebase delays, and measure the new shift time.
3. Bound the work done per `pb_poll` (a byte or a few bytes per pass) if the shift is still long enough to hurt step deadlines or e-stop.
4. Add a rule and a test that no shift, ADC read, LED update or USB write happens while PB11 is high.
5. Update `PB_TUNE_HEAD_SHIFT_TIME_US` from an estimate to a measured value.

## Acceptance criteria

- Shift time measured and recorded; data on the head verified correct by a known test pattern.
- No missed step deadline caused by the shift (measured).
- The new rule is enforced by a transcript or contract test; all suites pass; CI builds.

## Out of scope

Heater pulse changes, protocol changes.

## Handoff on completion

Include the measured shift time and the head clock chosen. Next: Sprint 7.
