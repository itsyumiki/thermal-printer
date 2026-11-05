# Timer and timing architecture across Pinbridge and stock thermal-printer firmware

Date: 2026-11-05

Scope: consolidated engineering notes from the Pinbridge firmware, the older stock thermal-printer firmware analysis, and the timing experiments discussed on 2026-11-04/05. This is a documentation-only snapshot. It does not change firmware, reserve peripherals, or endorse an unmeasured timing design.

## 1. Executive summary

There are several different mechanisms that have been called “timers” in this project, but they do different jobs:

- **SDK high-priority software timer / TIMER1-backed scheduler:** currently paces Pinbridge stepper callbacks. The API is asked for 1 ms, but the measured callback cadence is about 2 ms. TIMER1 must not be claimed directly while this SDK timer is in use.
- **Pinbridge hardware TIMER3:** currently terminates a heater pulse by lowering PB11 in an interrupt. It is configured as a divided crystal-clock timer and is not currently the stepper clock.
- **SDK jiffies:** a coarse system tick used for elapsed-time bookkeeping, timeouts, ADC settling and watchdog-style logic. Measurements support 10 ms per jiffy.
- **Software busy delays:** calibrated instruction/NOP loops used for short waits. They are not independent clocks and are affected by instruction timing and interrupt execution.
- **Stock firmware TIMER2:** the analyzed stock image uses a hardware TIMER2 interrupt for the printhead heat strobe. The stock stepper pacing is instead driven by a 2 ms SDK timer callback that posts print/feed events.
- **TIMER0 and TIMER2 in the Pinbridge SDK environment:** TIMER0 is associated with SDK delay facilities and TIMER2 with SDK handshake use. They should not be treated as free without checking the exact SDK build and runtime users.

A proposed alternative was discussed: move stepper pacing onto TIMER3 and replace the heater's hardware shutoff with a synchronous busy delay, on the assumption that the main loop can do heater and communications work between motor steps. This is plausible only after defining what “stepper spins” means and quantifying the longest heater pulse. If TIMER3 interrupts advance the motor phase, a blocking delay with interrupts enabled can stretch heater pulses through ISR interference; with interrupts disabled it can delay motor steps and all other interrupts. A 30 ms heater slice is not a negligible gap in a millisecond-scale step train.

**Correction 2026-11-05: the TIMER3 /4, /16 and /64 inconsistency was a bug in the old PTIME test, not evidence that the timer itself is misclocked.** The old test could count one compare twice (see section 3). Its `/4`, `/16` and `/64` totals are not valid calibration results; the `/256` run was close to one second but is too short to resolve the PRD-versus-PRD+1 question. The replacement binary TBP `TIMETEST` counts ISR deltas only. The corrected hardware run passes (section 3, corrected hardware table: all four ratios within 0.25 % of 1.0, `isr == n`, about 23.98 MHz). None of these timing figures is a PB11 scope measurement.

## 2. Timer ownership and use in Pinbridge

### 2.1 SDK high-priority timer: stepper pacing (TIMER1-backed)

In `firmware/apps/pinbridge/src/app.c`, Pinbridge registers the step callback with:

- `sys_hi_timer_add(NULL, pb_step_timer, 1)`
- the timer is retained while `core.step_active` is true and deleted when motion is no longer active.

The callback increments `core.step_timer_callbacks`, checks the emergency-stop state, and calls `pb_step_tick(&core)`. The core tick advances the stepper phase, updates movement counters/state, and drives the motor phase GPIOs through the HAL.

Important distinction: the requested API period is 1 ms; the measured wall-clock callback interval is about 2 ms. The API argument is not proof of actual 1 ms motor phase changes. Scheduler latency, timer implementation and callback execution all matter.

The source comments and prior experiment establish a critical resource constraint: the SDK high-priority timer implementation uses TIMER1. A direct TIMER1 configuration attempt caused the SDK stepper callback to stop firing (`PDIAG: timer registered, ticks=0`). Therefore:

- Do not configure TIMER1 directly while relying on `sys_hi_timer_add`.
- Treat the SDK timer and TIMER1 as one owned resource, not as independent resources.
- If the stepper is moved to a direct hardware timer, first identify a different safe timer and remove or replace the SDK timer path deliberately.

### 2.2 Hardware TIMER3: the timebase (Sprint 2, 2026-11-05)

> Status: **code and simulation only.** Built in CI and tested against a simulated timer; not yet run on hardware. The sections below that describe the old one-shot pulse path are historical (it was removed in Sprint 2).

TIMER3 is now a free-running microsecond clock (`pb_tb_us()`, exposed to the core as the `micros` HAL hook and `pb_micros()`).

| Item | Value | Why |
|---|---|---|
| Source / prescale | 24 MHz oscillator, /4 (SDK `irflt.c` index 1) | 6 ticks per us, resolution 1/6 us |
| PRD | 60000 | one wrap per exactly 10000 us, fits the 16-bit register, whole microseconds per period |
| Wrap interrupt | every 10 ms; `tb_wraps++`, then clear pending (`CON = BIT(14)\|...`) | also the period Sprint 3 wants for the heater deadman |
| `micros()` | `(wraps + pending) * 10000 + CNT / 6`, u32, wraps every 71.6 min | always subtract (`now - start`), never compare raw values |
| `CON` | `(0b10<<2) \| (1<<4) \| BIT(0) \| BIT(14)` | oscillator, /4, plain timer mode (not BIT(1): IR capture), clear pending |

Trade-offs considered. /16 with the same PRD would wrap every 40 ms and read at 0.67 us; /64 and /256 only lose resolution; /1 is not in the SDK table. A 10 ms wrap costs one short ISR per 10 ms (cost not yet measured, see section 3 once the owner has run it). The wrap ISR also bumps `core.heater_isr_count`, which the TIMER3 TIMETEST rows still use.

Read algorithm (`src/pb_timebase.h`, shared by firmware and the C harness): read wraps, counter, pending flag, counter again, wraps again; retry if the wrap count changed or the counter went backwards. A wrap whose ISR has not run yet (pending set) is added, so a late ISR never makes time step back by 10 ms. Limit: interrupts masked for more than one period lose a wrap.

`hal_delay_us` waits on `pb_tb_us()`. Its spin count is bounded (16 passes per us plus 256); if the timebase does not advance, it finishes with the old software loop, sets `timebase_stalled` and counts `core.timebase_stalls`. The per-call floor is the cost of one `pb_tb_us()` call and is measured by the DELAY2 row.

**Interim heater pulse.** `hal_heater_pulse_start` asserts PB11, waits in-line on `pb_tb_us()`, and forces PB11 low on every exit. `hal_heater_pulse_cancel` (USB RX task, e-stop) clears the armed flag and PB11 and no longer touches the timer. The in-line wait is also bounded by the jiffy counter (`usec/10000 + 3` jiffies), so a dead TIMER3 cannot hold PB11 high. This is not a safety timer; Sprint 3 moves the pulse into the executor with a deadman in the wrap ISR. `heater_poll_ends` and `heater_last_cnt/con` are no longer written.

**TIMETEST and the timebase.** The four TIMER3 rows borrow the timer: the wrap ISR is told to stop the timer on each compare (as the old heater ISR did), the run proceeds exactly as in Sprint 1, and the timebase is restarted from its saved wrap count afterwards. `micros()` therefore loses the length of such a run. New source 8 (`TIMEBASE`) busy-waits until `micros()` has advanced `ms * 1000` us and reports the elapsed count as `n`; the host gates it at 1 %.

Open assumption, to be confirmed by the TIMEBASE row on hardware: the counter reloads by itself at PRD in plain timer mode. If it does not, the wrap count would not advance and the TIMEBASE run would show it.

### 2.2a Historical: the one-shot heater pulse path (removed)

Before Sprint 2, `PB_HEATER_TIMER = JL_TIMER3` (/16, 1.5 ticks/us, 43.7 ms maximum) was programmed per pulse and its compare ISR drove PB11 low, with a pending-flag poll and a 2000 us software margin as fallbacks. Sprint 1 measured that clock correctly (ratios 1.0002-1.0014).

### 2.3 Jiffies: coarse elapsed time

Pinbridge's `hal_ms()` returns `jiffies * 10u`. The jiffy measurement reported 10.00 ms per jiffy (approximately 10.01 ms in the test output).

Jiffies support coarse scheduling and elapsed-time logic, including:

- timeouts and heartbeat/link supervision;
- ADC settling (the ADC path waits ten jiffies after registering a channel);
- sensor polling and other millisecond-scale state machines;
- coarse heater/duty-window and watchdog bookkeeping.

Jiffies are not a suitable replacement for a sub-millisecond or microsecond pulse timer. Their 10 ms granularity is much coarser than heater edge timing.

### 2.4 Software delay helper: `hal_delay_us`

> Sprint 2: `hal_delay_us` now waits on the timebase (section 2.2); the NOP loop below is only the fallback (`soft_delay_us`). The 881 / 940 / 5.2 us figures are the **before** numbers.

The Pinbridge HAL implements `hal_delay_us` as a calibrated NOP loop:

```c
unsigned loops = (us * 11u + 5u) / 10u;
for (n = 0; n < loops; n++) {
    __asm__ volatile("nop");
}
```

The comment records a hardware calibration on 2026-11-02. The earlier loop estimate was substantially wrong; the current calibration uses roughly 1.1 loop iterations per requested microsecond.

The old PTIME test reported (superseded by the `TIMETEST` baseline in section 3: 940.2 us and 5.216 us per call on build `a3270e1c`):

- requested 1,000 us: about 881.4 us per call;
- requested 2 us: about 4.965 us per call, including call/loop overhead.

This demonstrates why short requested waits are dominated by fixed overhead and why a software loop should not be treated as a precise clock. Interrupt execution can extend the observed wall time if interrupts remain enabled. Disabling interrupts to make the loop more repeatable would have system-wide consequences and is not automatically safe.

The helper is used for short protocol/edge waits and the bounded heater fallback. It is explicitly not certified as a heater safety timer.

### 2.5 TIMER0 and TIMER2 in the Pinbridge SDK environment

The current Pinbridge source comments identify:

- TIMER0: SDK short blocking delay timer;
- TIMER2: SDK handshake timer.

These are reasons not to casually repurpose either peripheral. They are not a complete proof of every SDK build's timer ownership. Before any future allocation, inspect the exact SDK configuration and all active call sites/interrupt registrations in the firmware image being built.

## 3. PTIME timing experiment: reported results and interpretation

The PTIME diagnostic compared several timing sources. The user-provided bench results are reproduced below.

| Source | Requested wall time | Measured wall time (mean ± SD; min–max) | Ratio | Initial verdict |
|---|---:|---:|---:|---|
| TIMER3 /16 | 1,000 ms | 656.8 ± 115.2 ms; 560.9–840.8 | 0.657 | Fast, about 1.52× |
| TIMER3 /4 | 1,000 ms | 835.2 ± 189.2 ms; 512.0–981.0 | 0.835 | Fast, about 1.20× |
| TIMER3 /64 | 1,000 ms | 740.8 ± 55.0 ms; 700.6–801.1 | 0.741 | Fast, about 1.35× |
| TIMER3 /256 | 1,000 ms | 1,000.7 ± 0.2 ms; 1,000.5–1,000.9 | 1.001 | Matches |
| SDK step callback | 1,000 ms | 1,999.7 ± 0.6 ms; 1,999.1–2,000.6 | 2.000 | About 2× slow |
| Jiffies | 1,000 ms | 1,000.1 ± 0.2 ms; 999.9–1,000.3 | 1.000 | Matches |
| Software delay, 1,000 us repeated | 1,000 ms total target | 881.4 ± 2.4 ms; 879.4–885.1 | 0.881 | About 1.13× fast |
| Software delay, 2 us repeated | 1,000 ms total target | 2,482.6 ± 1.1 ms; 2,480.6–2,483.3 | 2.483 | About 2.48× slow |

Additional reported details:

- TIMER3 /16 implied an oscillator estimate of 37.36 MHz when 24 MHz was assumed; compare IRQ count was 16 per run.
- TIMER3 /4 implied 30.41 MHz; compare IRQ count was 83 per run.
- TIMER3 /64 implied 32.54 MHz; compare IRQ count was 7 per run.
- TIMER3 /256 implied 23.98 MHz; compare IRQ count was 2 per run.
- Jiffies cross-checks were consistently about 10.00–10.02 ms per jiffy.
- SDK step callback measured 2.000 ms per callback despite the 1 ms request.

Interpretation and caveats:

1. The SDK step callback result is strong evidence that the currently registered 1 ms request does not produce a 1 ms callback cadence on this measured build/device. It is about 2 ms.
2. Jiffies provide a useful coarse wall-time reference and appear stable in this test.
3. TIMER3 prescaler results are internally inconsistent. The /256 result happens to match the 24 MHz assumption, but the other prescalers do not. Do not infer a corrected oscillator value from any one prescaler result.
4. The diagnostic can observe both ISR counters and timer pending flags. If those paths are not mutually exclusive, one physical event may be counted twice. Compare IRQ counts therefore need careful interpretation.
5. Timer mode, PRD semantics, clock source selection, prescaler encoding, interrupt latency, test-loop boundaries and counter rollover may all affect the derived values.
6. A scope or logic analyzer measuring PB11 high time is the decisive measurement for heater pulse duration. The PTIME wall-clock test is a diagnostic clue, not a substitute for measuring the output pin.

### Correction (2026-11-05): the TIMER3 rows were a test bug

The PTIME TIMER3 loop counted a compare in two places: through the ISR counter (`heater_isr_count`) and through the pending flag (CON bit 15). When the loop saw the flag before the ISR ran, it counted the period and restarted the timer; the ISR then still ran, stopped the restarted timer and was counted again. One compare was counted twice and a period was cut short, so the run reached its target count in less wall time. The pre-fix numbers show it directly: at /16, 25 counted periods were made of 16 ISRs plus 9 flag-only counts, and the wall time was 656.8 ms, about 16 x 40 ms.

Recomputed from the reported per-run ISR counts alone (one compare is one ISR), with the nominal 24 MHz periods of 10 / 40 / 100 / 500 ms:

| Row | ISR count x period | Mean wall | Wall / ISR time | Implied oscillator |
|---|---:|---:|---:|---:|
| TIMER3 /4 | 83 x 10 ms = 830 ms | 835.2 ms | 1.006 | 23.85 MHz |
| TIMER3 /16 | 16 x 40 ms = 640 ms | 656.8 ms | 1.026 | 23.39 MHz |
| TIMER3 /64 | 7 x 100 ms = 700 ms | 740.8 ms | 1.058 | 22.68 MHz |
| TIMER3 /256 | 2 x 500 ms = 1,000 ms | 1,000.7 ms | 1.001 | 23.98 MHz |

These are derived from the old run and are approximate: some periods were truncated by the stray ISR restarts, and /64 and /256 have only 7 and 2 periods, so one period of quantization is 14 % and 50 %. They support a 24 MHz crystal at every prescaler and the same CON encoding (source `0b10`, prescale index 1 / 2 / 3 / 8), but they replace nothing: the measurement of record is the corrected run below.

What changed in the test (`hal_time_test` in `app.c`):

- Only the ISR is counted: `n` grows by the change in `heater_isr_count` and by nothing else, and the pending flag is not read. A static contract test (`test_timetest_timer3_loop_counts_each_compare_once`) keeps it that way.
- The compare IRQ stays enabled, so the test also proves the interrupt that ends a heater pulse fires on every period. `isr` in the end line should equal `n`, and the host reports `isrMatchesN`; the host also reports the implied oscillator from the reported tick count, divider and wall time.
- Every restart (the ISR stops the timer, the loop starts it again from zero) adds the ISR entry and loop latency to that period, so n periods read slightly long. This bias is not measured; the 2 % gate allows 200 us per 10 ms period at /4. If it turns out to matter, the next step is a masked-IRQ, flag-only variant reported separately.
- A dead compare IRQ ends in `STATUS FAILED` with the TIMETEST timeout argument (the watchdog is cleared while waiting), rather than returning a misleading count or causing a watchdog reset.

### Corrected hardware table (2026-11-05, owner run)

Measured on the printer by the owner, 2026-11-05 about 11:30 (Europe/Istanbul), firmware build `ac07f950` (the build id HELLO reports; commit `ac07f95`), through the Bun host's stdin console with `TIMETEST` at 1,000 ms. One run per source. "Wall" is the host's clock between the printer's RUNNING and DONE frames (both written straight to USB), so it includes a millisecond or so of USB and host-scheduling jitter. This is a host-side measurement of the timer clock, not a PB11 scope measurement.

| Source | Counted `n` | `isr` | Jiffies `dj` | `ticks` | Wall | Wall / want | Implied oscillator | Jiffy |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| TIMER3 /4 | 100 | 100 | 100 | 6,000,000 | 1002.5 ms | 1.0025 | 23.94 MHz | 10.02 ms |
| TIMER3 /16 | 25 | 25 | 100 | 1,500,000 | 1001.0 ms | 1.0010 | 23.98 MHz | 10.01 ms |
| TIMER3 /64 | 10 | 10 | 100 | 375,000 | 1001.0 ms | 1.0010 | 23.98 MHz | 10.01 ms |
| TIMER3 /256 | 2 | 2 | 101 | 93,750 | 1000.9 ms | 1.0009 | 23.98 MHz | 9.91 ms |

Result: Sprint 1 acceptance is met on hardware (every ratio within 2 %, implied oscillator about 24 MHz, `isr` equal to `n` and to the expected 100 / 25 / 10 / 2).

- **The double count is gone.** `isr == n` on every row, where the old test counted some compares twice. The compare interrupt fires once per period at all four prescalers, and the CON encoding (source `0b10`, prescale index 1 / 2 / 3 / 8) is right.
- **The crystal is 24 MHz.** The /16, /64 and /256 rows imply 23.98 MHz, within 0.1 % of 24; the earlier derived figures of 22.7-23.9 MHz came from the buggy run.
- **Restart bias is small.** The /4 row restarts the timer 100 times and reads 2.5 ms (0.25 %) long, about 25 us per restart, which is the ISR entry plus loop latency this section predicted. The other rows are within 1 ms, which is the resolution of this method, so a bias there cannot be separated from jitter.
- **Jiffies are 10 ms.** 9.91-10.02 ms per jiffy; `dj` is 100 or 101 because the run does not start on a jiffy edge.
- **Not answered by this run:** whether the counter period is PRD or PRD + 1 (invisible at these large PRD values; it only matters for the stock heater's PRD of 45), the STEP, JIFFY, DELAY and DELAY2 sources (not run here), and the PB11 pulse width, which still needs a scope.

Gotcha when repeating this by pasting frames into the console by hand: the host's 1.5 s STAT keepalive uses the printer's frame ids too, so a pasted frame whose id was already consumed gets `ACK` plus an empty `DONE` (the firmware's replay of the stored state) and does not run. The first attempt on 2026-11-05 looked exactly like that. Paste the frames back to back, or use the web UI buttons below, which pause the keepalive for the run. The host does not yet handle this for console frames.

Web UI: the "Timer self-test" panel (`host/pinbridge/bun/public`) runs each source five times through binary `TIMETEST` and shows the mean, spread, ratio, implied oscillator, `isr` against `n`, and per-unit cost for the step tick, jiffy and delay sources. Use it to re-run the table above and to measure the STEP, JIFFY, DELAY and DELAY2 sources.

### Full TIMETEST baseline (2026-11-05, owner run, build `a3270e1c`)

All eight sources, five runs each, 1,000 ms target, from the web UI "Timer self-test" panel (the host pauses the keepalive for each run). Wall is the host's clock between the RUNNING and DONE frames, so it carries about a millisecond of USB and scheduling jitter. This is a host-side measurement, not a PB11 scope measurement. Build `a3270e1c` is the HELLO build id of commit `a3270e1`.

| Source | Wall (mean +- sd; min..max) | Ratio | Notes |
|---|---:|---:|---|
| TIMER3 /16 | 1001.1 +- 0.3 ms; 1000.5..1001.4 | 1.001 | 23.97 MHz, 25 compare IRQs per run (counted 25) |
| TIMER3 /4 | 1001.4 +- 1.8 ms; 998.2..1002.3 | 1.001 | 23.97 MHz, 100 IRQs (counted 100) |
| TIMER3 /64 | 1000.7 +- 0.0 ms; 1000.6..1000.7 | 1.001 | 23.98 MHz, 10 IRQs (counted 10) |
| TIMER3 /256 | 1000.2 +- 0.6 ms; 999.0..1000.6 | 1.000 | 24.00 MHz, 2 IRQs (counted 2) |
| Step tick | 1999.8 +- 0.8 ms; 1998.9..2001.0 | 2.000 | 2.000 ms per callback (assumed 1); jiffy cross-check 10.03 ms |
| Jiffies | 1000.3 +- 0.1 ms; 1000.2..1000.5 | 1.000 | 10.00 ms per jiffy |
| `delay(1000 us)` | 940.2 +- 2.0 ms; 938.2..942.5 | 0.940 | 940.2 us per call |
| `delay(2 us)` | 2608.0 +- 86.8 ms; 2512.6..2672.7 | 2.608 | 5.216 us per call |

Reading:

- **TIMER3 passes at every prescaler** (ratios 1.000-1.001, `isr == n`). The residual 0.2-1.4 ms is about the host timing resolution and does not separate a per-restart bias from jitter. PRD versus PRD + 1 is still not answered: at PRD 37500-60000 it is a 0.002 % effect, and the restart-per-compare design adds a latency (about 25 us per the first run) that is comparable to the stock 30 us tick, so this test cannot resolve it even at PRD 45. That needs a free-running mode or a scope.
- **The step callback is 2.000 ms, by design of the SDK.** The jiffy cross-check agrees (199-200 jiffies per 1000 callbacks), and it matches the stock analysis in section 4: TIMER1 runs at PRD 12000 on 24 MHz / 4, a fixed 2.000 ms, and `drv_timer1_update_period` ignores the requested period. The `1` passed to `sys_hi_timer_add` has no effect.
- **The host prints "OUT OF GATE" for STEP, DELAY and DELAY2.** That is a label problem: the 2 % gate was defined for the TIMER3 sources only, but `summarizeTimeTest` applies it to all eight. Treat these three as baselines.
- **`hal_delay_us` is not a stable clock.** Fitting `t = a + b x loops` to the two delay rows (loops = 1100 and 2) gives about 3.5 us fixed per call and about 0.85 us per loop iteration (two-point fit, approximate). The code assumes 0.91 us per iteration. The loop source has not changed since 2026-11-02, yet the old PTIME build measured 881.4 us and 4.965 us per call, which fits about 3.4 us fixed and 0.80 us per iteration. The same loop therefore ran about 6.7 % differently between two builds, so it cannot be calibrated once and trusted. `delay(2 us)` is also bimodal across runs (runs at 2.51 s and 2.67 s, about 0.32 us per call apart); the cause is unknown (cache or flash alignment and ISR load are guesses).
- **Safety-relevant consequence.** The heater pulse fallback loop in `app.c` counts `spent += 2u` per `hal_delay_us(NULL, 2u)` call, but a call costs at least 5.2 us. Its `usec + 2000 us` bound is therefore at least 2.6x longer in real time than intended (a 30 ms pulse: about 83 ms instead of 32 ms if TIMER3 were dead). DERIVED from this baseline, not measured. The normal path (TIMER3 ISR) is not affected. The path is being replaced in Sprints 2-3; see the Sprint 3 note.

## 4. Stock thermal-printer firmware: different allocation and timing model

The stock firmware analysis is static Ghidra/disassembly work, not a live hardware measurement. It describes a different firmware image and must not be mistaken for the current Pinbridge configuration.

### 4.1 Stock stepper pacing

The analyzed stock application registers `app_2ms_adc_and_print_tick` using `usr_timer_add(..., 2, ...)`. The SDK header documents the period argument in milliseconds.

The callback increments a divider counter and posts a print/feed event once the counter reaches `g_print_tick_div`. The code clamps the divider to at least 3. Thus the minimum nominal event-post interval is:

- 2 ms timer callback × 3 callbacks = about 6 ms per print/feed event;
- about 166.7 event posts per second at the minimum divider.

This is timer-side event posting, not a guarantee that the event handler starts or finishes exactly every 6 ms. Queue latency, scheduler delay and the amount of work in the event handler can stretch the actual motor cadence.

In print mode, one row is processed over four print events/buffers. Each event advances the motor phase. At the minimum divider, four events nominally span about 24 ms, but that is a scheduling-derived estimate, not a measured paper speed.

### 4.2 Stock phase routine busy loops

The stock stepper phase functions call a helper with an argument of 1,000 after writing their GPIO phase pattern. Disassembly shows a decrement-to-zero loop. The argument is an iteration count, not 1,000 microseconds and not a hardware timer period.

The stock print path also bit-bangs 384 data bits (48 bytes) and uses additional small busy-loop arguments around DATA and CLK edges. Those loop counts cannot be converted directly to microseconds without validated core-clock and instruction timing. GPIO access and function-call overhead add further duration.

Therefore, stock motor cadence is principally governed by the periodic callback/event scheduling, while the 1,000-iteration loop is a local phase settle delay. Do not copy the stock loop count into Pinbridge as a timing constant.

### 4.3 Stock heater strobe: TIMER2

The analyzed stock firmware uses a dedicated hardware TIMER2 interrupt for the printhead heater strobe:

- initialization derives a period from a constant of 144,000 and a prescaler table;
- the first fitting prescaler is /16;
- the derived PRD is 45;
- assuming a 24 MHz source divided by 16, the timer clock is 1.5 MHz and 45 counts correspond to roughly 30 us per interrupt;
- the ISR controls PB11 subject to print, paper, temperature and latch-pending conditions.

The ~30 us interval is derived from static register setup and the assumed timer clock. It has not been verified with a scope in the cited analysis. The stock ISR increments a heat counter and compares it to a requested threshold; the main path nominally drives PB11 high for approximately `max(T-1, 0)` timer intervals, with a separate cutoff around 200 ticks. At about 30 us per interval, the cap is approximately 6 ms.

The stock arrangement therefore keeps heater pulse timing independent from the print-event/stepper scheduler. Pinbridge's current TIMER3-based heater cutoff follows the same broad architectural idea—hardware interrupt turns the strobe off—but uses a different timer allocation and currently has unresolved calibration.

## 5. The proposed alternative discussed: TIMER3 for stepper, busy delay for heater

The idea was:

1. assign TIMER3 to a periodic stepper tick;
2. advance the motor phase from the TIMER3 interrupt;
3. pulse the heater synchronously from the main/command loop using a busy delay;
4. perform communications and other work between stepper updates.

This can be a viable architecture, but the exact meaning of “stepper spins” changes the answer.

### 5.1 If TIMER3 ISR advances the stepper phase

The stepper cadence can be independent of main-loop scheduling, provided the ISR is short and the timer is correctly configured. The heater delay then has two choices:

- **Interrupts remain enabled during the busy wait:** TIMER3 continues to run the stepper, but every step ISR consumes time while the heater delay is in progress. The heater pulse ends only when the busy-wait code reaches its end, so ISR execution can make PB11 high longer than requested. Other interrupts can also extend the delay.
- **Interrupts are disabled during the busy wait:** the busy wait becomes less exposed to ISR preemption, but TIMER3 cannot service stepper ticks during the heater pulse. Those ticks are delayed, lost, or need explicit catch-up behavior. USB/communications and safety-related interrupt response can also be delayed.

Neither choice gives both a precise heater pulse and uninterrupted stepper cadence for free.

### 5.2 If the main loop itself drives motor phases

A tight main-loop step routine could schedule heater and communications work between motor updates. This avoids a high-rate stepper ISR, but the cadence is now coupled to all work performed in the loop. A blocking heater delay creates a deliberate gap in motor updates. USB processing and protocol work also introduce jitter unless carefully bounded.

### 5.3 Pulse duration is the key constraint

The current experimental maximum heater slice is 30,000 us (30 ms). A 30 ms blocking section is large relative to a 1–2 ms step tick and the nominal 6 ms stock event interval. If a heater slice can actually reach tens of milliseconds, the design must explicitly define whether motion pauses, whether a pulse can be split, or whether heating is deferred until a safe motor boundary.

If the actual intended pulse widths are only a few milliseconds or less, a main-loop delay might be operationally acceptable for a deliberately slow/paused print mode, but this must be measured and validated against print quality and thermal safety. It should not be assumed from the API name or requested duration.

### 5.4 Architectural trade-off

| Concern | TIMER3 stepper + busy-delay heater | Hardware heater cutoff + separate stepper pacing |
|---|---|---|
| Step cadence | Good if ISR owns phase updates; main-loop version has jitter | Depends on stepper scheduler, but heater pulse does not block it |
| Heater pulse width | Includes software overhead and possibly ISR time | Hardware compare/interrupt can terminate independently |
| Interrupt load | Stepper ISR at every motor update | Heater ISR only at pulse end (plus step scheduler) |
| Main-loop availability | Blocked for the heater delay | Usually remains available |
| Long heater slices | Can pause motion or distort pulse width | More naturally supported, subject to timer range/calibration |
| Complexity | Fewer hardware roles, but tricky interrupt interaction | More explicit timer ownership and independent deadlines |
| Safety | Busy delay is not an independent fail-safe cutoff | Hardware cutoff is a stronger independent shutoff mechanism, if configured and verified |

A timer-driven stepper and a hardware-timed heater pulse are not mutually exclusive in principle, but they require two independent timing resources. TIMER3 cannot serve both functions simultaneously unless the design deliberately multiplexes deadlines with a single timer and proves that scheduling model.

## 6. Current timer/resource map

| Resource | Current Pinbridge role / known SDK role | Guidance |
|---|---|---|
| SDK high-priority timer (`sys_hi_timer_add`) | Stepper callback; implementation uses TIMER1 | Keep using the SDK API or replace it intentionally; do not directly claim TIMER1 underneath it |
| TIMER0 | SDK short blocking delay support | Treat as occupied unless exact SDK build proves otherwise |
| TIMER1 | Underlying SDK high-priority timer resource | Do not configure directly while SDK timer is active |
| TIMER2 | SDK handshake use in this Pinbridge environment; stock firmware uses TIMER2 for heater strobe | Do not assume it is free in Pinbridge; stock allocation is a different firmware image |
| TIMER3 | Pinbridge free-running microsecond timebase (/4, 10 ms wrap ISR); SDK IR-filter/key-driver use in general SDK, absent from this app | App-reserved since Sprint 2; the heater no longer has its own one-shot |
| Jiffies | 10 ms coarse system timebase | Use for millisecond-scale bookkeeping, not precise pulse edges |
| NOP busy loop | Short software waits and fallback delay | Use only where timing tolerance is understood; never treat as a certified hardware deadline |

## 7. What is established, what is inferred, and what remains unknown

### Established by source inspection

- Pinbridge stepper pacing currently uses `sys_hi_timer_add(..., 1)`.
- The step callback calls the portable core step tick and drives motor phase progression.
- Pinbridge heater shutoff currently uses TIMER3 and an ISR that drives PB11 low.
- Pinbridge's software delay is a NOP loop.
- Pinbridge's `hal_ms()` is derived from jiffies at 10 ms per jiffy.
- Stock firmware analysis found a 2 ms SDK callback, a minimum divider of 3, a separate TIMER2 heater ISR, and software decrement loops in the motor phase functions.

### Established by the reported bench test

- The measured Pinbridge SDK step callback interval is 2.000 ms, despite the 1 ms request, and the jiffy cross-check agrees (section 3, full baseline).
- Jiffies are 10.00 ms each.
- The software delay helper does not match requested durations: 0.940 at 1000 us and 2.608 at 2 us on build `a3270e1c`, and the same loop measured differently (0.881 and 2.483) on the older PTIME build.
- The old TIMER3 `/256` run was close to 1 second, but the old `/4`, `/16`, and `/64` results are invalid because the test could count a compare once from the ISR and again from the pending flag (section 3, Correction). Do not treat the old table as calibration evidence. The corrected TIMER3 table is in section 3 and passes.

### Not yet established

- Actual PB11 pulse widths for Pinbridge at each requested duration.
- Whether the counter period is PRD or PRD + 1 (invisible at the large PRD values `TIMETEST` uses and not resolvable by the restart-per-compare design; it would be a 2.2 % effect on the stock heater tick).
- Whether TIMER3 can be cleanly reassigned to stepper pacing in the exact SDK configuration without another hidden consumer.
- The maximum heater pulse duration required in normal printing after experimental limits are restored or reselected.
- Whether heater busy-wait blocking would be acceptable for the real motor cadence and USB/control responsiveness.
- Whether a single timer can safely multiplex stepper and heater deadlines without unacceptable jitter.

## 8. Recommended measurements before changing the design

1. **Measure PB11 directly.** Use a scope or logic analyzer to capture requested heater widths (at minimum short, medium and maximum supported values). Record pulse width, repeatability, and whether PB11 always returns low.
2. **Measure stepper phase edges.** Capture one motor phase GPIO while running a known step train. Compare requested interval, observed interval, jitter and missed/delayed transitions.
3. **Repeat timer calibration with an independent reference.** Measure TIMER3 output/pulse duration at more than one prescaler, and confirm the actual timer clock source and PRD counting convention.
4. **Audit TIMER3 ownership in the exact build.** Check SDK configuration and linked users/IR-filter registration, not just generic SDK source comments.
5. **Bound the heater delay.** Establish the true maximum per-pulse duration and the permitted timing error before considering a software delay.
6. **Test interrupt policy explicitly.** If evaluating busy-delay heating, compare interrupts-enabled and interrupts-disabled behavior while measuring both PB11 and motor phase edges. Include USB receive and emergency-stop response in the test plan.
7. **Preserve an independent heater-off path.** Any architecture change must define what forces PB11 low if the command task stalls, the main loop wedges, or an unexpected exception occurs.

## 9. Practical conclusion

The current design has a clear separation of concerns: the SDK timer paces stepper work, TIMER3 independently ends heater pulses, jiffies handle coarse time, and NOP loops handle short software waits. The TIMER3 `/4`, `/16`, and `/64` inconsistency was a test bug: the old test could count one compare from both the ISR and pending flag. The corrected loop now counts ISR deltas only, and the replacement diagnostic is binary TBP opcode `F0` (`TIMETEST`) because the legacy plaintext `PTIME` command was removed. The corrected four-row TIMER3 table has been measured on hardware and passes (section 3), so a misclocked TIMER3 is unlikely to explain the blank prints (the PB11 pulse width still needs a scope), and Sprint 1's hardware acceptance is met. Sprint 0’s stock pulse estimates are derived from static analysis; the owner’s direct PB11/motor-phase scope capture is also still pending.

Moving the stepper to TIMER3 and using a busy delay for the heater is a reasonable experiment to discuss, not yet a demonstrated improvement. It exchanges independent heater shutoff for blocking software timing and creates a direct conflict between pulse duration and stepper service unless the motion is intentionally paused or the interrupt behavior is carefully designed. The next decisive evidence is a simultaneous measurement of PB11 and a motor phase pin under representative print load.

## 10. Source locations

### Pinbridge

- `firmware/apps/pinbridge/src/app.c` — SDK step timer registration, jiffies conversion, software delay, TIMER3 heater setup/ISR, pulse start/cancel and the binary TIMETEST diagnostic.
- `firmware/apps/pinbridge/src/pb_core.h` — HAL timer/delay interfaces, stepper state/counters, heater pulse hooks and core scheduling state.
- `firmware/apps/pinbridge/src/pb_printer.h` — printer row execution and heater/stepper sequencing logic.
- `docs/bugs/2026-11-04-heater-blank-prints-timing.md` — heater timing investigation, known uncertainties and suggested measurements.
- `docs/journal.md` — contemporaneous project notes (contains pre-existing user edits; not modified as part of this document).

### Stock firmware analysis

- `~/projects/thermal-printer/re/docs/stepper-and-printhead-timing-2026-11-02.md` — static Ghidra analysis of stock stepper event pacing, busy loops, printhead bit-banging and TIMER2 heater strobe.
- `~/projects/thermal-printer/re/docs/printhead-protocol-and-heating.md` — related printhead protocol/heating analysis.
- `~/projects/thermal-printer/fw-AC63_BT_SDK/include_lib/system/timer.h` — SDK timer API period documentation referenced by the stock analysis.

---

This document is an engineering record, not a safety certification. Timer allocation and heater behavior must be revalidated against the exact firmware build and measured hardware outputs before relying on them for thermal or motion safety.
