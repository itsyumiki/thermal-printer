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

**Correction 2026-11-05: the TIMER3 /4, /16 and /64 inconsistency was a bug in the PTIME test, not in the timer.** The test counted one compare twice (see section 3, "Correction"). Counting the compare interrupts alone, all four prescalers agree with a 24 MHz oscillator to within a few percent (retroactively, from the reported per-run ISR counts); a corrected run on hardware is still pending. The TIMER3 /16 numbers are therefore plausible pulse widths, but they are still not measured on PB11: PB11 should be measured directly with a scope or logic analyzer before heater timing is trusted or changed.

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

### 2.2 Hardware TIMER3: Pinbridge heater pulse deadline

Pinbridge currently assigns:

- `PB_HEATER_TIMER = JL_TIMER3`
- `PB_HEATER_TIMER_IRQ = IRQ_TIME3_IDX`

Initialization stops and resets TIMER3, registers `pb_heater_timer_isr`, and explicitly unmasks its interrupt source.

The heater pulse start path asserts PB11, programs the timer period and starts TIMER3. The ISR stops/acknowledges TIMER3, drives PB11 low directly through the port output register, clears `heater_timer_armed`, and increments the heater ISR diagnostic counter. Lowering PB11 in the ISR means the shutoff does not depend on the command task being scheduled at that instant.

The implementation's original clock assumption is:

- oscillator: 24 MHz
- prescaler: /16
- nominal timer tick: 24 MHz / 16 = 1.5 MHz
- nominal tick duration: about 0.667 us, or 1.5 ticks/us
- 16-bit period register: 65,535 counts, about 43.7 ms at that assumed rate
- the current test-only slice maximum of 30,000 us corresponds to 45,000 ticks at 1.5 ticks/us.

The code also contains a pending-flag polling path and a bounded software-delay fallback/safety margin. The margin constant is 2,000 us. These are defensive completion checks; they do not make the timer's clock calibration correct. The ISR, pending-flag observation and polling can also complicate diagnostic event counts if the same timer event is counted through more than one path. That is exactly what the first PTIME test did; it now counts the ISR only (section 3).

The app comments note that TIMER3 is used by the SDK IR-remote key driver in the general SDK, but this app does not include that IR-remote feature and repurposes TIMER3. This is an app-specific ownership decision, not a claim that TIMER3 is universally unused in every SDK application.

### 2.3 Jiffies: coarse elapsed time

Pinbridge's `hal_ms()` returns `jiffies * 10u`. The jiffy measurement reported 10.00 ms per jiffy (approximately 10.01 ms in the test output).

Jiffies support coarse scheduling and elapsed-time logic, including:

- timeouts and heartbeat/link supervision;
- ADC settling (the ADC path waits ten jiffies after registering a channel);
- sensor polling and other millisecond-scale state machines;
- coarse heater/duty-window and watchdog bookkeeping.

Jiffies are not a suitable replacement for a sub-millisecond or microsecond pulse timer. Their 10 ms granularity is much coarser than heater edge timing.

### 2.4 Software delay helper: `hal_delay_us`

The Pinbridge HAL implements `hal_delay_us` as a calibrated NOP loop:

```c
unsigned loops = (us * 11u + 5u) / 10u;
for (n = 0; n < loops; n++) {
    __asm__ volatile("nop");
}
```

The comment records a hardware calibration on 2026-11-02. The earlier loop estimate was substantially wrong; the current calibration uses roughly 1.1 loop iterations per requested microsecond.

The PTIME test reported:

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

- Only the ISR is counted: `n` grows by the change in `heater_isr_count` and by nothing else, and the pending flag is not read. A static contract test (`test_ptime_timer3_loop_counts_each_compare_once`) keeps it that way.
- The compare IRQ stays enabled, so the test also proves the interrupt that ends a heater pulse fires on every period. `isr` in the end line now equals `n`, and the host reports `isrMatchesN` (false for output from the old firmware, which also makes the host rescale the implied oscillator to the ISR count).
- Every restart (the ISR stops the timer, the loop starts it again from zero) adds the ISR entry and loop latency to that period, so n periods read slightly long. This bias is not measured; the 2 % gate allows 200 us per 10 ms period at /4. If it turns out to matter, the next step is a masked-IRQ, flag-only variant reported separately.
- A dead compare IRQ now ends in `err timeout` (the watchdog is cleared while waiting) rather than a wrong count or a watchdog reset.

Corrected hardware table: pending. The owner needs to flash a CI build of this change and run `PTIME` for all four TIMER3 sources; paste the table here. Acceptance: every row's ratio within 2 % of 1.0, an implied oscillator near 24 MHz, `isr` equal to the expected period count (100 / 25 / 10 / 2 for the default 1,000 ms).

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
| TIMER3 | Pinbridge hardware heater pulse deadline; SDK IR-filter/key-driver use in general SDK, absent from this app | Currently app-reserved for heater; could be reassigned only as a deliberate architecture change |
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

- The measured Pinbridge SDK step callback interval is about 2 ms, despite the 1 ms request.
- Jiffies are about 10 ms each.
- The software delay helper does not match requested durations exactly, especially at very short waits.
- TIMER3 /256 gave a 1-second wall-clock result close to 1 second in that run. The other prescalers disagreed only because the test counted compares twice (section 3, Correction); their per-run ISR counts agree with 24 MHz within a few percent.

### Not yet established

- Actual PB11 pulse widths for Pinbridge at each requested duration.
- The corrected PTIME TIMER3 table on hardware (the old table is superseded, the new run is pending).
- Whether the counter period is PRD or PRD + 1 (invisible at the large PRD values PTIME uses; it would be a 2.2 % effect on the stock heater tick).
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

The current design has a clear separation of concerns: the SDK timer paces stepper work, TIMER3 independently ends heater pulses, jiffies handle coarse time, and NOP loops handle short software waits. The TIMER3 /4, /16 and /64 inconsistency was explained as a PTIME test bug (compare counted twice; see the 2026-11-05 correction in section 3) and is fixed in code and mock/contract tests, but it is not closed: the corrected PTIME table has not been measured on hardware yet (Sprint 1 gate). The remaining open issue is the lack of direct PB11 pulse measurements (the Sprint 0 scope plan, not yet run by the owner), not the conceptual allocation.

Moving the stepper to TIMER3 and using a busy delay for the heater is a reasonable experiment to discuss, not yet a demonstrated improvement. It exchanges independent heater shutoff for blocking software timing and creates a direct conflict between pulse duration and stepper service unless the motion is intentionally paused or the interrupt behavior is carefully designed. The next decisive evidence is a simultaneous measurement of PB11 and a motor phase pin under representative print load.

## 10. Source locations

### Pinbridge

- `firmware/apps/pinbridge/src/app.c` — SDK step timer registration, jiffies conversion, software delay, TIMER3 heater setup/ISR, pulse start/cancel and PTIME diagnostics.
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
