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

## Implementation status (2026-11-05; not yet hardware-accepted)

Implemented and pushed in commit `fb4e707` (2026-11-05); CI run [42006282931](https://github.com/itsyumiki/thermal-printer/actions/runs/42006282931) passed both the host checks and firmware build. Not flashed:

- Removed `heater_pulse_start/active/cancel` and the executor's asynchronous-pulse pending state. The executor now asserts PB11 and polls the shared `micros()` clock until the requested slice duration, checking the emergency-abort flag and a coarse jiffy fallback while PB11 is high.
- Added `heater_deadman_arm/disarm` as the minimal HAL contract for the independent shutoff. The TIMER3 wrap ISR compares the current microsecond time with the armed deadline and directly clears PB11 if the executor has not done so. Disarm atomically clears PB11 before clearing the armed flag.
- The deadman deadline is the requested pulse deadline plus a 100 µs setup allowance. TIMER3's 10 ms wrap period means the independent cutoff may occur up to about 10.1 ms after the requested deadline; this is a safety bound, not the precision pulse timer. The core also exits on a coarse jiffy bound if the timebase stops.
- Removed the obsolete `tbp_heater_pending`, `heater_poll_ends`, and `heater_last_cnt/con` fields. Added `heater_deadman_trips` and `heater_over_deadline`; the latter counts a jiffy fallback or a pulse more than 1 ms over its requested duration.
- Extended the race harness to check simulated 1 ms, 2 ms and 3 ms pulse widths, abort at pulse start and mid-pulse. Extended the timebase harness to exercise deadman expiry with a simulated wedged executor and across u32 wrap.

### Local verification

- C transcripts against the real pin table: 40/40 pass.
- `pb_race_test`: 2059 cases pass, including simulated 1 ms / 2 ms / 3 ms pulse-width accounting and abort cases.
- `pb_led_test`: pass.
- `pb_timebase_test`: pass, including deadman expiry and u32-wrap cases.
- Bun `run check`: 90 tests pass; Prettier passes.
- `git diff --check`: pass.

### Still required before closing Sprint 3

- CI firmware build passed on `fb4e707` (run 42006282931); no local JieLi toolchain is available.
- Owner-run scope captures at three pulse widths, verifying PB11 width within 5% and immediate low on e-stop.
- Owner-run forced-loop-wedge test confirming the independent ISR cutoff within the documented ~10.1 ms maximum after the requested deadline.
- Hardware evidence and final review of the counters. No hardware has been touched.

### Review fix (2026-11-05, before any flashing)

A review of `fb4e707` found a unit bug in the executor's jiffy fallback: it compared a millisecond difference (`pb_now`, which the real HAL advances in 10 ms jiffies) against `on / 10000 + 3`, where `on` is microseconds. That threshold is 10x too small. On hardware it would have cut a 30 ms slice at the first jiffy tick (within 10 ms) and a 2.5 ms slice about a quarter of the time, and each cut counted as `heater_over_deadline`. The race harness missed it because it holds `now_ms` still during a pulse.

- Fix: `pb_heater_jiffy_expired(elapsed_ms, on_us)` in `pb_core.h` trips at `on_us / 1000 + 30` ms (three jiffies of slack), so a pulse that ends on time never trips it and a dead-timebase pulse is cut within about 40 ms of the request.
- Test: `pb_race_test` now models a jiffy clock with the pulse starting at every 250 us phase of the jiffy, for 11 pulse widths from 100 us to 60 ms. It requires no trip at or before the deadline (plus 1 ms) and a trip within 50 ms of it, and it checks that the old formula would have failed (it fails 248 cases). 2499 cases pass.
- Integration test: `pb_race_test` kind `K_HEATED_JIFFY` runs full heated rows through the executor with `millis` quantized to 10 ms jiffies (20 variants: 10 jiffy phases, 3 ms and 14 ms slices) and requires `heater_over_deadline == 0` and no deadman trip. With the old formula 16 of the 20 fail.
- Also from the review: the independent ISR deadman checks only at TIMER3 wraps, so it can overshoot a wedged pulse by up to about 10.1 ms. For a stock-sized pulse (0.7-2.2 ms) that is 5x or more; it is a backstop, and the executor loop is the precise timer.
- The fallback is still coarse and is the only protection if TIMER3 itself dies (the ISR deadman stops with it).
