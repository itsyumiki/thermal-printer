# Sprint 2 — Free-running timebase and accurate delays

**Size:** M  
**Depends on:** Sprint 1  
**Goal:** Give the core a hardware-timed `micros()` and make `hal_delay_us` honest, so every later deadline uses real time.

## Read first

- `pb_heater_timer_init`, `hal_delay_us`, `hal_heater_pulse_*` in `app.c`
- `pb_hal` in `pb_core.h` and the mock HALs in `tests/c/` (`pb_test.c`, `pb_sim.c`, `pb_race_test.c`, `pb_led_test.c`)
- SDK `irflt.c` TIMER3 prescale table and the BR23 `JL_TIMER` registers

## Tasks

1. Choose and document the timebase configuration: prescaler (e.g. /4 for fine resolution or /64 for long wrap), PRD, and whether a slow wrap interrupt is used. Prefer one timer at about 10 ms period whose ISR increments a wrap count and doubles as the heater deadman (Sprint 3). Write the trade-offs down.
2. Add `uint32_t (*micros)(void *)` to `pb_hal`, with a consistent read of wrap count and counter. Update the four test HALs and add a simulated clock to the C harness.
3. Rewrite `hal_delay_us` to wait on the timebase, with a bounded fallback if the timer is not running.
4. Extend `TIMETEST` with a TIMEBASE source and re-run the DELAY sources.
5. Record the interrupt and ISR cost of the chosen design.

## Acceptance criteria

- On hardware: timebase vs host wall time within 1 %, `delay(1000)` and `delay(2)` within 2 % plus a documented per-call floor.
- Existing heater behavior is unchanged (the old pulse path still works).
- All suites pass; CI builds.

## Out of scope

Moving the heater or stepper off their current mechanisms.

## Handoff on completion

Include the chosen configuration, measured resolution and wrap, and the measured per-call overhead. Next: Sprint 3.
