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

1. Record owner-confirmed pin/peripheral mappings and functions from the existing WebUI/scripts; separately track sensor transfer-function calibration and timing measurements.
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

## Implementation checkpoint — 2026-11-02

Status: **owner-confirmed pin/peripheral mappings; fail-closed software protections implemented; Sprint 3 remains in progress for sensor acquisition/calibration and safety-limit validation**.

### Owner hardware confirmation — 2026-11-02

The owner confirms that all pin mappings and physical functions—including buttons, LED/light, stepper, printhead signals, and heater—were already verified using the previous WebUI and scripts. The earlier statements calling these mappings hypotheses were stale and are superseded by this confirmation. This confirms physical identity/function; it does not by itself provide firmware sensor conversion curves, fault thresholds, or calibrated timing/duty budgets.

### Implemented in the current worktree

- Firmware INFO now truthfully advertises `burn_max_us=0`, zero heater timer/guard/duty/shift limits, sensor capability bits `0`, and unavailable raw temperature/battery values. The current build therefore rejects every nonzero-burn ROW with `ERR RANGE` at payload offset 48 before accepting it.
- Legacy `PHEAT`, `PEVENT` with nonzero heat, and direct PB11-high paths are blocked in firmware. Generic `MOD PB11` accepts only `lo`; `SET PB11=1` is rejected. Safe-state, ESTOP, FLUSH, watchdog and cancellation paths force PB11 to output-low rather than high impedance.
- Motor outputs remain released until motion starts and are released after completion. COILS-on and nonzero idle-hold configuration are rejected until coil thermal limits are measured; COILS-off remains available.
- CONFIG heartbeat interval and link timeout are range-checked, stored, and applied. Heartbeat is opt-in (default off); link timeout cancels running/queued work with reason `04`. STAT reports the running command ID and current output flags.
- The executor contains fail-stop handling for missing required sensor capabilities and duty-limit violations, plus a rolling-window duty wait. Those paths cannot enable heating in the current build because `burn_max_us=0` and all sensor capabilities are unavailable.
- Both WebUIs currently treat PC4/PC5/PB3 as raw inputs because the new Pinbridge firmware path has not yet implemented/validated the corresponding sensor interpretation; this is not a claim that the owner has not verified the physical pin functions. The Bun UI only permits heatless feed tests; real-mode Python WebUI heat actions and PB11-high requests fail closed.

### Verification in this checkpoint

- Bun host suite: 21 tests / 74 assertions, including no-send rejection for heat-bearing rows and successful heatless-row transport.
- Python/C suite: 35 tests, including C transcripts for blocked heater paths, CONFIG range/effective values, heartbeat EVENT, link-timeout cancellation, duplicate heatless ROW side effects, and motor output release.
- `./scripts/check-pinbridge.sh` and `git diff --check` pass.

### Exit blockers — do not enable heat yet

1. Implement actual sensor acquisition and conversion for the owner-confirmed temperature, battery-divider, and paper-sensor nets; document calibration curves/ratios and fault thresholds in the new firmware path.
2. Implement actual sensor acquisition, validation, filtering/debounce, and calibrated thresholds. A capability bit must not be enabled until the associated sampling path is real.
3. Measure and owner-approve heater timer resolution, maximum burn, guard gap, row-shift time, duty window/budget, and motor-coil hold limits.
4. Add tests and controlled validation for sensor faults, overtemperature, low battery, paper absent, ESTOP, link loss, and watchdog behavior before changing the zero heater limits. Owner-confirmed pin functionality does not substitute for validating these safety interlocks in the new firmware path.

The present work intentionally disables heater operation. Physical pin/peripheral functions are owner-confirmed; sensor acquisition/conversion, thresholds, firmware timing and safety interlocks still need validation. Do not set `PB_TBP_BURN_MAX_US` or sensor capability bits nonzero as a workaround.

### Editable tuning header — 2026-11-02

`firmware/apps/pinbridge/src/pb_tuning.h` now centralizes the previous WebUI/demo timing starting points and the calibration values that still need to be carried into the new firmware path. The provisional timing defaults are head clock 2 us, data setup 1 us, latch 2 us, heater request 1500 us nominal (6000 us legacy request ceiling only), and 1 ms / 6 ms stepper starting points. The 56 kOhm thermistor pull-up is recorded; thermistor R0/Beta, ADC reference, battery divider ratio, and sensor thresholds remain explicitly unset where this repo does not contain a grounded numeric value.

These tuning inputs do **not** alter the active fail-closed gate: sensor capabilities, heater timer readiness, burn tick/max, guard, duty window/budget, and shift timing advertised to TBP remain zero. Existing host-supplied timing arguments remain explicit command inputs; macros are the central source for editable firmware policy/defaults, not an implicit override of command fields.

### Sensor math foundation — 2026-11-02

`src/pb_sensors.h` adds pure helper functions for calibration-readiness checks, NTC divider resistance, beta-equation temperature conversion in milli-degrees C, and battery-divider voltage, with C tests for valid arithmetic and invalid/unset inputs. These helpers do not sample hardware, calculate temperature, filter readings, or set capability bits. With current zero/unset calibration inputs, readiness remains false and the heater gate remains closed. Next work is to add measured ADC sampling/freshness and validated thermistor temperature conversion, then fault-state handling and tests before advertising any sensor capability.
