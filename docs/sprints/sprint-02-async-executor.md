# Sprint 2 — Bounded asynchronous executor and ESTOP

**Size:** M  
**Depends on:** Sprint 1  
**Goal:** Remove long-running printer work from the protocol receive path and make emergency stop independent of queue/executor progress.

## Read first

- `docs/thermal-protocol-spec.md` sections 5–8 and 12
- Firmware `pb_tbp_line`, command dispatch, stepper timer/task and HAL adapter
- Existing watchdog and local PB1/PA1/PC7 state machine
- Sprint 1 handoff and the C transcript harness

## Completion report — 2026-11-02

Status: **complete against software acceptance criteria**. Physical timing remains unverified and is explicitly deferred to the controlled validation sprint.

### Delivered

- Static four-command FIFO; INFO advertises `queue_depth=4`, `max_window=4`, and STAT reports actual queue usage/depth.
- ROW head shifting is chunked across poll iterations; MOVE and DWELL are poll-driven and do not wait for full duration in `pb_tbp_line`.
- Accepted jobs receive immediate ACK. QUEUED is emitted after 1000 ms of waiting and repeated each second; long jobs emit RUNNING and 1-second PROGRESS updates. Duplicate history follows QUEUED/RUNNING and terminal states.
- FLUSH, CRC-valid ESTOP, watchdog timeout, and USB detach cancel active/queued jobs with reasoned CANCELLED statuses. Cancelled MOVE/DWELL commands do not later emit DONE.
- A higher-priority RX task validates complete ESTOP frames, drives PB11 low immediately, and raises an executor-abort flag. The step timer stops at its next tick; the command task subsequently emits protocol ACK/DONE/CANCELLED responses. HELLO clears the fast-abort gate after session reset.
- Host supports four accepted outstanding commands while sending only one not-yet-ACKed frame at a time. It uses advertised credits, discards unsent later IDs after rejection, and fails closed on ambiguous ERR responses after ACK.
- C transcript tests cover FIFO ordering, queue full/credit recovery, delayed QUEUED, long DWELL/MOVE progress, FLUSH/ESTOP/disconnect cancellation, watchdog cancellation, duplicate ROW/MOVE side-effect suppression, fast-abort gating, and CRC/length/opcode validation for fast ESTOP frames. Bun fake-transport tests cover queue credits, rejection backpressure, retries and sequence-error safety.

### Verification

- `cd host/pinbridge/bun && bun run check` — PASS, 18 tests / 68 assertions, Prettier clean.
- `cd host/pinbridge && .venv/bin/python -m unittest discover -s tests -v` — PASS, 34 tests, including strict C transcript builds.
- `./scripts/check-pinbridge.sh` and `git diff --check` — PASS.
- GitHub Actions [run 41506821421](https://github.com/itsyumiki/thermal-printer/actions/runs/41506821421) — PASS for commit `ad848edf4368e59c2232fcaf3e7c0df5de52c4d0`; both host checks and firmware build/package succeeded.
- Artifact: `pinbridge-br23-41506821421` (650,993 bytes). No local firmware build was run.

### Evidence boundary and next sprint

No firmware was flashed and no physical ESTOP latency was measured. The fast RX path is source- and build-validated, with unit coverage for frame validation and executor abort behavior, but physical response timing and USB-detach timing remain unverified. Sprint 3 must add firmware-enforced safety gates and truthful sensor capabilities before heater-driven printing can be considered usable.

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
