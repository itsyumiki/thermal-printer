# Sprint 3 — Heater pulse in the executor, on the timebase

**Size:** M  
**Depends on:** Sprint 2 (and Sprint 0 for the pulse target)  
**Goal:** Time every heater slice from the timebase inside the executor, with an interrupt deadman, replacing `heater_pulse_start/active/cancel`.

## Note carried over from Sprint 1 (do not reuse the old bound)

The old in-line fallback in `heater_pulse_start` (`app.c`) loops `spent += 2u` around `hal_delay_us(NULL, 2u)` and gives up at `usec + PB_HEATER_PULSE_MARGIN_US`. Measured `delay(2)` is about 5.2 us per call (plus the `CON` register read), so `spent` undercounts real time by at least 2.6x. If TIMER3 stopped counting, a 30 ms pulse would be held for roughly 83 ms instead of 32 ms (DERIVED from the baseline, not measured). This path is being replaced, so it is not fixed separately. The replacement must bound PB11-high time against the timebase (`micros()`), jiffies, or the deadman interrupt, never against a count of assumed-microsecond delays.

## Read first

- Stage 3 of `pb_tbp_exec_poll` in `pb_core.h`, `pb_legacy_heat`, `pb_heater_off`
- `docs/bugs/2026-11-04-heater-blank-prints-timing.md`
- The safety gates in `pb_tuning.h`

## Tasks

1. Make the core fallback path the real path: PB11 high, a poll loop on `micros()` until the deadline, PB11 low. While PB11 is high, do nothing but check the deadline, the e-stop flag and the abort flag.
2. Add the deadman: the timebase interrupt forces PB11 low if the heater has been on longer than the request plus a small margin. It must work if the main loop wedges.
3. Keep every existing gate (temperature, battery, paper, duty, per-slice max) unchanged and tested.
4. Retire `heater_pulse_*` from the HAL and `PHDIAG`'s old fields; add counters for deadman trips and over-deadline pulses.
5. Add transcripts: pulse accounting with the simulated clock, abort mid-pulse, deadman trip with a wedged loop.

## Acceptance criteria

- Scope: PB11 width within 5 % of the request at three widths, and low immediately on e-stop.
- A forced loop wedge is cut off by the deadman within its margin (owner-run test).
- No heater wait loop counts assumed microseconds; every bound is read from the timebase, jiffies or the deadman.
- No gate regressions; all suites pass; CI builds.

## Out of scope

Raising any heater limit, changing step handling.

## Handoff on completion

Include scope captures, deadman margin, and the counters' meaning. Next: Sprint 4.
