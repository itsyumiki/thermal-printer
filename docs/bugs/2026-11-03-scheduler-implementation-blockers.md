# Pinbridge scheduler follow-up — 2026-11-03

Status: **row scheduling and asynchronous heater shutoff are implemented in source and covered by portable-core simulation; hardware timing acceptance remains unverified.** No firmware build or hardware run was performed.

## Hardware timer implementation

- The BR23 SDK's `JL_TIMER_TypeDef` exposes `CON`, `CNT`, `PRD`, and `PWM` registers. SDK `timer_pwm_init` configures the crystal source with `(0b10 << 2)`, a divide-by-four prescaler with `(0b0001 << 4)`, and the counter enable bit. The SDK IR timer ISR acknowledges its interrupt with bit 14 and registers `IRQ_TIME3_IDX`; its setup shows bit 1 enables timer interrupts.
- Pinbridge now reserves `JL_TIMER1` / `IRQ_TIME1_IDX` for heater pulse deadlines. The timer runs from the 24 MHz crystal divided by four (6 MHz, about 0.167 us per count); the 2,500 us software pulse cap fits within 15,000 counts. The timer ISR stops/acknowledges the timer and directly drives PB11 low without waiting for the command task.
- Pulse start and cancellation execute with local interrupts disabled while timer and output state are updated. `pb_heater_off`, abort, disconnect and fault paths cancel the timer and force PB11 low. Timer-start failure fails the row closed rather than falling back to a blocking pulse on the production adapter.
- TIMER1 is reserved for this app's heater driver; don't enable SDK TIMER1 PWM users concurrently. The exact timer interrupt latency and clock tolerance still need hardware measurement. No build or hardware timing test was run in this change.

## Scheduler changes

- Host `print-solid` sends one 54-byte `ROW` per logical image row, not four copies of the same bitmap. Each ROW contains four feed steps, a 6 ms effective period and four heater slices, so the firmware shifts and latches the bitmap once.
- The host keeps a bounded window of compatible rows in flight. This lets firmware extend the continuous step train across row boundaries; row progress is counted only when the corresponding command reaches terminal completion.
- The firmware records the absolute step range allocated to a row and gates heater slice `i` until the corresponding completed step edge. For four slices across four steps, each slice waits for one distinct edge. An incompatible existing feed train is waited out before head shifting begins; retry state avoids charging the rolling heat budget repeatedly.
- The C race test checks exactly 384 shift clocks, one latch edge, four heater rising edges per row, each heater edge at its corresponding completed motor step, consecutive-row ordering, timer-start failure, and cancellation while a simulated pulse is active. It also asserts that the async pulse path never calls the HAL delay for a full heater-pulse duration.
- Temperature, battery, paper-presence, overtemperature, per-slice, rolling-duty and abort gates are unchanged. The 2,500 us per-slice cap is retained; it is a conservative software ceiling, not a hardware-validated thermal limit.

## Remaining acceptance work

The portable simulation verifies state and ordering, not real TIMER1 register behavior, pulse-off jitter, GPIO electrical latency, physical darkness, or thermal behavior. Firmware compilation and physical timing acceptance have intentionally not been run. Do not raise pulse limits to compensate for timing defects.
