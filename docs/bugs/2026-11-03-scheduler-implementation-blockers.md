# Pinbridge scheduler follow-up — 2026-11-03

Status: **not hardware-ready; autonomous four-event row scheduler remains blocked on verified timer semantics and printhead behavior.** This note records the code and SDK evidence reviewed during the follow-up.

## Verified from checked-in source

- `app.c` registers motion through `sys_hi_timer_add(NULL, pb_step_timer, 1)` and removes it with `sys_hi_timer_del(step_timer_id)`. The callback directly calls `pb_step_tick`, which updates motor phase and GPIO. The timer API's implementation/header is not present in the checked-out `ac6956c-sdk` tree (that checkout contains generated build metadata and a sample app, not the vendor system timer implementation). The period, callback context, jitter, cancellation guarantees and in-flight callback behavior therefore cannot be verified from the available SDK source.
- Head data and clock use PB10/PB9; PB8 is pulsed after the 384-bit shift; PB11 is the heater output. This verifies the firmware's GPIO sequence, **not** whether the head has an independently protected output-latch register or whether shifting while heating can safely prepare the next row. No board schematic or printhead datasheet is present in the repository.
- The TBP ROW executor shifts one 384-bit bitmap, pulses PB8, then executes blocking PB11 pulses using `pb_wait_us`; its HAL implementation is a calibrated NOP loop. Motion progresses from a separate timer callback and can continue while the executor blocks, but heater shutoff is not hardware-timed.
- Host `print-solid` currently submits four separate ROW commands per logical row, repeats the bitmap and latch sequence, uses a 15 ms step period, and dispatches up to three workers. This does not establish four step-edge-synchronized heater events, and queued acceptance/completion is not a substitute for a verified timing schedule.

## Safe independent correction in this change

The temporary 10,000 µs per-slice cap was removed. Firmware, host protocol validation and UI now use a conservative 2,500 µs maximum per heater slice, close to but still slightly above the stock-derived 2.22 ms event estimate. The existing 10,000 µs ROW total remains a total-budget ceiling only; the executor must divide it into slices that individually pass the cap. These values are source-level policy, not validated thermal limits or a claim that pulse duration is safe on physical hardware. Existing temperature, battery, paper and rolling-duty gates remain in place.

## Why the full scheduler was not changed speculatively

A safe nonblocking heater scheduler needs a documented and testable hardware timer/compare facility with bounded shutoff latency and known callback/interrupt behavior, or another proven SDK mechanism. None is available in the checked-out SDK source. Reusing the existing 1 ms step timer for a 0.72–2.22 ms heater pulse would quantize pulse widths and could miss the off deadline; replacing it with the same calibrated delay loop would remain blocking. Similarly, latching once and shifting the next row early cannot be asserted safe without confirming the printhead's shift/output storage and latch semantics.

Do not flash or use this change as evidence of correct print speed, synchronized four-event heating, or restored print darkness. Before implementing the autonomous scheduler, obtain the vendor timer implementation/header or a supported timer API reference, and the board/head schematic or authoritative printhead timing specification. Then verify the actual timer period/jitter and PB8/PB9/PB10/PB11 waveforms on a scope. Until then, retain the conservative cap and treat heated printing as experimental.
