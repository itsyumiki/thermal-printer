# Pinbridge scheduler follow-up — 2026-11-03

Status: **software row sequencing is implemented and simulated; exact heater shutoff remains blocking and requires a finer-grained supported timer facility.** No firmware build or hardware run was performed.

## Timer findings

- `app.c` registers motion with `sys_hi_timer_add(NULL, pb_step_timer, 1)` and cancels it with `sys_hi_timer_del`. The project SDK copy is `../sdk-pinbridge/include_lib/system/timer.h`. Its `sys_hi_timer_add` macro calls `usr_timer_add(..., priority=1)`; the API documentation explicitly gives `msec` as milliseconds and describes the callback as asynchronous on the hardware-timer timebase. The low-power `sys_s_hi_timer_add` uses priority 0 and its period can stretch when low-power ticks are skipped; the app correctly uses the priority-1 variant for active motion.
- The SDK header declares the timer API but the matching timer implementation is not included in the checked-out source set. Exact callback interrupt/task context, jitter, in-flight deletion behavior and worst-case latency cannot be established from the declaration alone.
- SDK examples use `JL_TIMER0` as a 1 us counter, but the examples poll its completion flag in a blocking loop. This is not a nonblocking heater-shutoff facility and must not be presented as one. The millisecond `sys_hi_timeout_add` family is too coarse to enforce stock-derived 0.72–2.22 ms pulses with suitable precision.

## Scheduler changes now implemented

- Host `print-solid` sends one 54-byte `ROW` per logical image row, not four copies of the same bitmap. Each ROW contains four feed steps, a 6 ms effective period and four heater slices, so the firmware shifts and latches the bitmap once.
- The host keeps a bounded window of compatible rows in flight. This lets firmware extend the continuous step train across row boundaries; row progress is counted only when the corresponding command reaches terminal completion.
- The firmware records the absolute step range allocated to a row and gates heater slice `i` until the corresponding completed step edge. For four slices across four steps, each slice waits for one distinct edge. An incompatible existing feed train is waited out before head shifting begins; retry state avoids charging the rolling heat budget repeatedly.
- The C race test now drives a heated ROW through the portable core and checks exactly 384 shift clocks, one latch edge, four heater rising edges, and that each heater edge occurs at its corresponding completed motor step. This is a simulation guarantee only; it does not establish physical timer jitter or pulse width.
- Temperature, battery, paper-presence, overtemperature, per-slice, rolling-duty and abort gates are unchanged. The 2,500 us per-slice cap is retained; it is a conservative software ceiling, not a hardware-validated thermal limit.

## Remaining implementation limitation

PB11 pulses still use the existing calibrated `pb_wait_us` delay loop, which blocks the command task while a pulse is asserted. The motion ISR can continue independently, but the pulse-off deadline is not hardware-enforced and the control-plane latency during a pulse is not fully nonblocking. The SDK sources inspected here do not expose a documented, usable sub-millisecond one-shot shutoff API with verified callback and cancellation semantics. Do not claim that requirement is solved by the 1 ms motion timer or the blocking `JL_TIMER0` examples.

The software simulation covers relative ordering, not actual event timing, GPIO electrical latency, timer jitter, darkness, or thermal behavior. A future timer implementation must make PB11 shutoff independent of the command poll loop and guarantee PB11 low on abort, disconnect, safety fault, timer failure, and new-row transitions. Do not raise pulse limits to compensate for timing defects.
