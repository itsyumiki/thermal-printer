# Sprint 3 — Firmware safety gates and honest telemetry

**Size:** M  
**Depends on:** Sprint 2 executor interfaces  
**Goal:** Ensure heater/motion behavior is governed by firmware-side interlocks and that INFO/STAT never imply sensors or safety checks that do not exist.

## Read first

- `docs/thermal-protocol-spec.md` sections 7–10 and 14
- Pin table and board mapping; ADC-related code and stock firmware research references
- `docs/stock-print-scheduler-handoff.md`
- Journal notes about measured heater-delay error and current experimental print settings

## Tasks

1. Verify physical pin/ADC mappings from source evidence and, where necessary, an owner-provided measurement. Treat PC4/PC5/PB3 hypotheses as unverified until proven.
2. Make INFO capability bits truthful. Missing/un-calibrated temperature, battery, or paper inputs must be reported unavailable, never fabricated as healthy values.
3. Implement a safe heater-enable gate: valid temperature measurement and within limit, no active overheat, valid row parameters, duty budget available, heater-off guard satisfied, and correct latch state. If required sensing is unavailable, fail closed for heater operations unless the owner explicitly approves a documented temporary policy.
4. Enforce compiled-in firmware hard limits for burn pulse, total duty window, step period, and any other agreed timing/range constraints. Host/UI validation is additional convenience, not the safety boundary.
5. Implement overheat wait/failure behavior and HOT/RESET/PAPER/HEARTBEAT events only when their trigger source and payload are real. Do not emit invented sensor readings.
6. Establish coil hold/release and link-loss behavior with conservative defaults; keep power-button shutdown behavior separate from protocol ESTOP and define their interaction without blocking the button path.
7. Add pure logic/unit tests for limits, sensor-invalid states, duty-window accounting, hot transitions, disconnect, and safe defaults.
8. Document calibration and measurement procedures; do not pick numeric thresholds by guesswork.

## Acceptance criteria

- All safety-critical limits are enforced in firmware, independently of WebUI inputs.
- Invalid/unavailable sensors cannot be mistaken for a healthy reading.
- Heater output defaults off after reset, session loss, parse error where applicable, ESTOP, and watchdog.
- Sensor capabilities and all event payloads are verified against real sources in code.
- Numeric safety values have a cited measurement or explicit owner decision; unresolved values remain blockers, not silently chosen defaults.
- CI build and tests pass. No flashing or heater test is performed by the agent.

## Out of scope

Print-quality tuning, image dithering, and UI feature expansion.

## Handoff on completion

List each confirmed pin/sensor mapping, safety constant and evidence, plus remaining hardware-only measurements. Next: Sprint 4 scheduler can rely only on these verified gates.
