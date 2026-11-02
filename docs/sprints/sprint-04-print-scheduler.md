# Sprint 4 — Coordinated row scheduler and continuous feed

**Size:** L, split into small commits if needed  
**Depends on:** Sprints 2–3  
**Goal:** Replace host-paced shift/latch/heat/step sequences with an MCU-owned job scheduler, preserving motion across row boundaries.

## Read first

- `docs/thermal-protocol-spec.md` sections 6–11 and 14
- `docs/stock-print-scheduler-handoff.md`
- `host/pinbridge/examples/print_matrix_square_ws_demo.py`
- Firmware print-head shift/latch, stepper phase/timer, and event/queue code
- Current hardware timing observations in `docs/journal.md`

## Tasks

1. Before coding, resolve spec assumptions: separate shift/latch register, shift duration, timer resolution, phase changes during shifts, row buffer ownership, four-event stock row accounting, dot order, and actual command-to-paper direction.
2. Implement ROW scheduling on the MCU: shift/latch/heat timing and step timer coordination must be owned by firmware, not WebSocket request cadence.
3. Pre-shift the next queued row only if hardware analysis proves shifting cannot corrupt the active latched row. Otherwise serialize it and report the actual lower throughput.
4. Preserve motor phase and coil state across consecutive ROW/MOVE commands; do not stop/restart between rows. Define behavior for direction changes and idle boundaries.
5. Implement effective timing quantization and report actual burn/period values plus COLD_START, STRETCHED, LATE, DUTY_WAIT, HOT_WAIT, and NO_HEAT only when conditions are detected accurately.
6. Add deterministic scheduler tests using a fake clock/HAL: 4-event accounting if confirmed, phase order/direction, queued row handoff, late shift, no-heat row, burn slices, continuous motion, empty queue, and cancel/ESTOP at boundaries.
7. Change the print demo to submit a multi-row job and consume progress/terminal status, not send one step operation per row/event. Keep a dry-run mode.

## Acceptance criteria

- A multi-row job is submitted without a host round trip per print event or motor transition.
- Motion phase progression stays continuous across adjacent rows when queued, and no deliberate stop/start is inserted at row boundaries.
- Scheduler timing and row accounting are testable independently of hardware and agree with the spec.
- Heater timing passes through Sprint 3 safety gates; there is no alternate unsafe print path.
- C/host tests and GitHub Actions firmware build pass. No local build or flash.

## Out of scope

Gray-level quality tuning, acceleration profiles, persistent calibration profiles, and claims about safe burn time without owner-run measurements.

## Handoff on completion

Record which stock behaviors were confirmed vs inferred, the timer resolution and shift-time evidence, scheduler test results, and any throughput limit. Next: Sprint 5 integrates UI queue/job progress with real asynchronous semantics.
