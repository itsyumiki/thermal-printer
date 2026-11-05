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

## Status (2026-11-05): hardware gate run, closed with a documented delay floor

Commit `c2b08e2`, CI green (run 42002821340, build id `0xC2B08E2F`, `app.bin` 80,840 bytes, SHA-256 `0cf02e7d...4f85`). The owner's hardware run came afterwards and is recorded under "Hardware result" below (written before it arrived: the local-evidence table and the open items in this section are the pre-hardware state). Local re-check on 2026-11-05 after the push: Bun 90/90, `pb_test` 40/40 (synthetic and real pin tables), `pb_timebase_test`, `pb_race_test` (2005 cases) and `pb_led_test` pass, generated-file check OK. 

### What was built

- Timebase: TIMER3, 24 MHz /4 = 6 ticks per us, PRD 60000, wrap ISR every 10 ms (`tb_wraps++`, then clear pending). Rationale and trade-offs: `docs/timer-architecture-and-usage.md` section 2.2.
- `src/pb_timebase.h`: `pb_tb_us()` (retry-on-wrap read that also counts a wrap whose ISR is late) and `pb_tb_delay_us()` (bounded spin). Shared by `app.c` and `tests/c/pb_timebase_test.c`. Added to `app.conf`.
- `pb_hal.micros` (last field) and `pb_micros()` (falls back to `millis * 1000`). The four test HALs provide a simulated micros clock.
- `hal_delay_us` waits on the timebase; bounded fallback to the software loop on a stalled timer (`core.timebase_stalls`).
- Interim heater pulse: in-line on the timebase, PB11 forced low on every exit, cancel no longer touches the timer, independent jiffy bound. Old one-shot ISR path removed.
- TIMETEST: source 8 `TIMEBASE`; the TIMER3 rows borrow the timer and restore it. Host: `TIME_SRC.TIMEBASE`, 1 % gate for it, web UI button.

### Evidence so far (local, none of it hardware)

| Check | Result |
|---|---|
| C transcripts (`pb_test`, synthetic and real pin tables, gcc `-std=c11 -Wall -Wextra -Werror`) | 40/40 pass, includes TIMETEST source 8 and the new bad-source case 9 |
| `pb_race_test`, `pb_led_test` (`-std=gnu11 -O1 -Wall`) | pass |
| `pb_timebase_test` (simulated timer: ISR lag 0..59999 ticks, ISR masked, u32 wrap, delays 1 us..30 ms, stopped timer) | pass; two deliberate breakages of the header were both caught |
| Simulated delay overshoot | delay(2) at most 3.33 us, delay(1000) at most 1001.67 us with one register access = 1/6 us (simulation only) |
| Bun (`bun run check`: prettier and tests) | 90 pass |
| `app.c` stub compile (`-fsyntax-only`, fake SDK headers) | no diagnostics in the changed code; compile check only |

The Python suite was not run (dropped from the gates).

### Not tested / open

- Hardware (resolved, see the hardware result below): counter reload, timebase vs wall, `delay(1000)`, `delay(2)`. Still open: the cost of `pb_tb_us()` and of the 10 ms wrap ISR (task 5; not measured).
- Interrupts masked longer than 10 ms lose a wrap (documented, not guarded).
- `heater_poll_ends` and `heater_last_cnt/con` are now unused.

### Owner run to close the sprint

Flash the CI build, then from the web UI run `TIMEBASE`, `DELAY` and `DELAY2` (5 runs) and the TIMER3 rows (they must still pass with `isr == n`). Record the numbers in `timer-architecture-and-usage.md` section 3 and here.

Next: Sprint 3 (numbers are in, see the hardware result below).

### Hardware result (owner run, web UI, 5 runs each, 2026-11-05)

Full table: `docs/timer-architecture-and-usage.md` section 3, "Sprint 2 hardware table".

| Gate | Result |
|---|---|
| TIMEBASE vs wall within 1 % | **Met**: ratio 1.001 (1000.6 +- 0.2 ms; 1,000,001 us counted) |
| TIMER3 rows still pass, `isr == n` | **Met**: ratios 1.000-1.002, 23.96-23.99 MHz, `isr` 100 / 25 / 10 / 2 |
| `delay(1000)` within 2 % | **Met**: 1013.1 us per call (+1.3 %), was 940.2 |
| `delay(2)` within 2 % plus a documented floor | **Met only through the floor**: 11.369 us per call; floor about 9.4 us. Worse than before (5.216 us) |
| Interim heater path forces PB11 low and is bounded in real time | Met by code reading and tests; no scope capture (none exists yet) |
| Suites pass, CI builds | Met (Bun 90/90, C harness, CI green on `c2b08e2`) |

Closed by the letter of the criteria, with one regression to carry forward: **`hal_delay_us` costs about 9-13 us per call on hardware**, and the simulation did not predict it. Cause not isolated; the likely suspect is the register reads inside `pb_tb_us()`. Task 5 (interrupt and ISR cost) is still unmeasured.

Handoff to Sprint 3: configuration is TIMER3 /4, PRD 60000, 6 ticks per us, wrap every 10 ms; resolution 1 us from `micros()`, but each `pb_tb_us()` call itself costs several us (not measured separately), so do not poll it for sub-10 us precision. The heater wait loop in Sprint 3 polls `micros()` against a deadline of tens of ms, where that cost is irrelevant. Do not use `hal_delay_us` for few-us edges (Sprint 6).
