# Sprint 2 — Bounded asynchronous executor and ESTOP

**Size:** M  
**Depends on:** Sprint 1  
**Goal:** Remove long-running printer work from the protocol receive path and make emergency stop independent of queue/executor progress.

## Read first

- `docs/thermal-protocol-spec.md` sections 5–8 and 12
- Firmware `pb_tbp_line`, command dispatch, stepper timer/task and HAL adapter
- Existing watchdog and local PB1/PA1/PC7 state machine
- Sprint 1 handoff and the C transcript harness

## Tasks

1. Define one bounded FIFO and one explicit running-command record. Advertise the actual queue depth and maximum outstanding window in INFO; never report capacity that the firmware does not have.
2. Separate parsing/acceptance from execution. ACK promptly after acceptance, then emit STATUS transitions with one terminal state per accepted queued command.
3. Implement queue credits, QUEUED/RUNNING/PROGRESS/DONE/FAILED/CANCELLED status, last-terminal tracking, and result history consistent with the final spec.
4. Implement FLUSH cancellation at a defined safe boundary and document whether the running ROW/MOVE can stop immediately or only at the next step boundary.
5. Make CRC-valid ESTOP bypass sequence/session/queue checks and promptly disable heater output and step timer. Queue-full and long-running work must not starve ESTOP parsing. Keep the response/cancellation ordering consistent with the spec.
6. Add tests for FIFO ordering, full queue, queue credit recovery, cancellation ordering, terminal uniqueness, long DWELL/MOVE progress, ESTOP while busy, and watchdog interaction.
7. Keep ISR work bounded. Do not perform UART formatting, heap allocation, or long delays in a timing-critical ISR.

## Acceptance criteria

- Receiving ROW/MOVE/DWELL never blocks parsing for the full operation duration.
- Queue depth/capabilities match measured static storage and runtime behavior.
- ESTOP turns the heater off through the highest-priority available path and is testable while the executor is busy/full.
- Every accepted command has exactly one terminal state; cancelled commands never later report DONE.
- Tests cover queue-full, flush, ESTOP and reset/session cleanup. CI firmware build succeeds.

## Out of scope

Thermistor calibration, print scheduler optimization, and browser styling. Do not claim physical ESTOP latency until measured on hardware.

## Handoff on completion

Document queue sizing, execution context, critical sections, cancellation boundary, and CI artifact SHA. Next: Sprint 3 safety/telemetry must gate execution through explicit executor interfaces.
