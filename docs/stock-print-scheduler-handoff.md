# Reproducing the stock printhead/stepper scheduler

Status: design/handoff note, 2026-11-02. No implementation or hardware test is implied by this document.

## Problem observed with the Pinbridge demo

`host/pinbridge/examples/print_matrix_square_ws_demo.py` currently performs a host-orchestrated sequence for each row: shift 48 bytes, latch, issue a heater pulse, then run a finite `PSTEP` operation for the row's motor transitions. It waits for each operation before sending the next. This creates a bursty print/move rhythm with explicit starts and stops. The owner reports the paper can stick during those pauses.

Do not try to fix this just by increasing `--row-steps` or changing a delay. The missing piece is a firmware-side print scheduler that coordinates head activity and motor phase changes on the MCU, instead of relying on WebSocket command boundaries as the timing source.

## What static analysis of the stock firmware supports

Reference: `~/projects/thermal-printer/re/docs/stepper-and-printhead-timing-2026-11-02.md`, especially “Additional stepper-driver timing trace”, “Stepper: stock eight-state half-step sequence”, and “Thermal printhead wire protocol”.

- The stock callback `app_2ms_adc_and_print_tick` at `0x01E0045A` is registered with a nominal 2 ms period.
- It clamps `g_print_tick_div` at RAM `0x13968` to at least 3 and posts one print/feed event after that many callback invocations. This gives a **nominal minimum event-post interval of 6 ms**, before queueing and execution delays. This is inferred from code, not a measured motor rate.
- Print event `0x122` shifts and latches one 48-byte/384-bit head buffer, then advances the stepper by one phase. The print-state machine uses four buffers containing the same row across four events, so one input row maps to four head operations, four heater windows, and four phase advances.
- The stock eight-state stepper phase sequence is already documented in the timing reference. Its local `1000` delay argument is a loop count, **not 1000 microseconds**.
- Head data/clock/latch bit-banging and heater strobe timing are performed by firmware. The heater strobe uses TIMER2; the head shift routine sets the pending-latch state and resets the heat-window counter. Do not replace these with USB/WebSocket `SET` calls as a timing mechanism.
- Feed/retract event `0x123` advances the motor once per event under its guards. Some counter behavior groups operations in fours, but exact public feed-distance units still need tracing.
- All timing above is static-analysis-derived. There are no oscilloscope measurements or calibrated paper-speed figures in this note.

## Implementation direction

1. Read the current Pinbridge source and tests before editing; the scheduler described here does not exist yet.
2. Add a **firmware-side autonomous print/feed operation** (name/protocol to be chosen after reviewing the current parser and WebSocket facade). Host sends a row/job and configuration once; firmware owns the sequence and reports progress/status. Do not require one host round trip per shift/latch/heat/step.
3. Model the stock sequencing explicitly: each logical row is consumed over four scheduled print events, each event handles the correct head buffer/latch/heater interaction and advances one motor phase. Verify buffer ownership and row progression against the disassembly before coding this as fact in the implementation.
4. Use the MCU timer/event scheduler for regular pacing. Keep the stepper's official eight-state phase order and avoid repeated finite `PSTEP` start/stop calls at row boundaries. Do not assume the nominal 6 ms post cadence means a phase change can safely interrupt an in-progress 384-bit shift; establish event/handler behavior and serialization first.
5. Preserve paper-present, active-job, valid-temperature, pending-latch and overheat gating as appropriate. Pinbridge currently lacks the stock thermal interlocks and is not a safe replacement for them. In particular, the full-width black demo's 6000 us per-row heater setting is experimental; do not increase it or treat it as a verified safe limit.
6. Keep low-level commands for diagnostics, but make the new scheduler the intended path for coordinated printing. Define cancellation, disconnect handling, motor release/coast behavior, busy status, and error reporting before adding a demo.
7. Add host-side tests for state transitions, four-event row accounting, motor phase order/direction, invalid job input, busy/cancel behavior and transcript/API status. Add firmware compile validation. No `cargo test`.
8. Do not flash or write hardware. Mark any build as not hardware-tested; the owner will decide whether to flash and test it.

## Acceptance criteria

- The demo can submit a multi-row job without sending separate WebSocket commands for every motor transition.
- Motor phase progression is timer/event driven and remains regular across logical row boundaries; no intentional stop/start is introduced between rows.
- The firmware coordinates row shift/latch/heat and phase movement in the recovered order, including four print events per logical row where verified by source.
- A status/cancel path behaves deterministically and the host can tell queued, running, complete, and failed states apart.
- Unit/transcript checks, syntax checks, and SDK build pass; report what was and was not tested. No claim of safe heater operation or physical success without owner-run hardware validation.

## Known unknowns to resolve before implementation

- Trace the full stock print-state machine and buffer-pop logic, including what happens when the event queue is late or data/paper is unavailable.
- Confirm whether a pending event can overlap a still-running head shift, or whether the timer merely posts work that is serialized elsewhere.
- Trace the BLE commands that set print speed/divider and feed distance; current evidence does not establish all command semantics.
- Determine the Pinbridge firmware's actual timer resolution and critical-section/interrupt constraints. Do not copy the stock 2 ms/6 ms values blindly into a different scheduler.
- Define how thermal sensor/overheat gating will be represented. Until it is implemented and validated, coordinated printing remains experimental.
