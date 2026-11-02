# Sprint 2 — Bounded asynchronous executor and ESTOP

**Size:** M  
**Depends on:** Sprint 1  
**Goal:** Remove long-running printer work from the protocol receive path and make emergency stop independent of queue/executor progress.

## Read first

- `docs/thermal-protocol-spec.md` sections 5–8 and 12
- Firmware `pb_tbp_line`, command dispatch, stepper timer/task and HAL adapter
- Existing watchdog and local PB1/PA1/PC7 state machine
- Sprint 1 handoff and the C transcript harness

## Current progress — 2026-11-02

Status: **in progress; do not mark Sprint 2 complete yet.**

Implemented in the current working tree:

- Static four-command FIFO; INFO advertises `queue_depth=4`, `max_window=4`, and STAT reports actual queue usage/depth.
- ROW head shifting is chunked across poll iterations; MOVE and DWELL are poll-driven and do not wait for full duration in `pb_tbp_line`.
- Accepted jobs receive immediate ACK. QUEUED is emitted after 1000 ms of waiting and repeated each second; long jobs emit RUNNING and 1-second PROGRESS updates. Duplicate history follows QUEUED/RUNNING and terminal states.
- FLUSH and CRC-valid ESTOP cancel the active and queued jobs with reasoned CANCELLED statuses. ESTOP bypasses session/sequence checks and enters STOPPED; the watchdog cancels active/queued work instead of allowing a stale executor to finish later.
- Host supports four accepted outstanding commands while sending only one not-yet-ACKed frame at a time. It uses advertised credits, resets credits on terminal states, discards unsent later IDs after a rejection, and fails closed on an ambiguous ERR SEQ after ACK.
- C transcripts cover FIFO ordering/credit recovery, queue full, delayed QUEUED, long DWELL/MOVE progress, FLUSH/ESTOP cancellation, duplicate ROW/MOVE side-effect suppression, watchdog cancellation, fast-abort gating, and CRC/length/opcode validation for the independent fast ESTOP scanner. Bun fake-transport tests cover queue credits, rejection backpressure, retry recovery and sequence-error safety.

Known acceptance gaps before completion:

- A higher-priority RX task now scans complete CRC-valid TBP ESTOP lines, cuts PB11 low directly, and raises an executor-abort flag; the step timer stops at its next tick. The command task later emits the protocol ACK/DONE/CANCELLED sequence. C tests exercise the executor-abort gate, but the USB scanner itself is only compile-validated and its physical latency remains unmeasured.
- Review FLUSH cancellation timing against the intended step-boundary contract. A C transcript now cancels a running MOVE and advances simulated time to prove it does not later emit DONE.
- Verify the SDK firmware build on the final commit and inspect the task-priority/stack changes. No firmware has been flashed and no physical ESTOP latency has been measured.

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
