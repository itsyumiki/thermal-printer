# Sprint 2 — Free-running timebase and accurate delays

**Size:** M  
**Depends on:** Sprint 1  
**Goal:** Give the core a hardware-timed `micros()` and make `hal_delay_us` honest, so every later deadline uses real time.

## Decisions (owner, 2026-11-05)

- The existing heater behavior may change. TIMER3 becomes the timebase, so the current one-shot pulse path (`heater_pulse_start/active/cancel`, the TIMER3 ISR, the `usec + margin` fallback loop) may be rewritten, replaced with a minimal interim path, or temporarily broken. Most of the heater and stepper timing is reimplemented in Sprints 3-5 anyway. Do not spend effort keeping the old pulse path intact.
- Whatever interim heater path exists must still force PB11 low and must not rely on a counted-microsecond bound (see the note in Sprint 3).
- The stepper is untouched here. It stays on `sys_hi_timer`, which is a fixed 2.000 ms tick (TIMER1, the requested period is ignored).

## Baseline to beat (Sprint 1, build `a3270e1c`)

| Clock | Measured |
|---|---|
| `hal_delay_us(1000)` | 940.2 us per call (ratio 0.940) |
| `hal_delay_us(2)` | 5.216 us per call (ratio 2.608), about 3.5 us fixed per call, bimodal between runs |
| TIMER3 oscillator | 23.97-24.00 MHz (ratios 1.000-1.001) |
| Jiffy | 10.00 ms |

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
- The interim heater path (whatever replaces the old one-shot) forces PB11 low and is bounded in real time; its behavior is otherwise allowed to differ from before.
- All suites pass; CI builds.

## Out of scope

Moving the heater pulse into the executor with a deadman (Sprint 3) and moving the stepper off `sys_hi_timer` (Sprint 5). Raising any heater limit.

## Handoff on completion

Include the chosen configuration, measured resolution and wrap, and the measured per-call overhead. Next: Sprint 3.
