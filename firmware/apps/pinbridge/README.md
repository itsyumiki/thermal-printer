# Pinbridge firmware

Pinbridge is an experimental TBP v1 thermal-printer firmware for JieLi AC6956C / AC635N (BR23), using USB CDC. **The wire protocol is hex-framed TBP only. Plaintext commands and plaintext replies are not supported.**

## Firmware layout

- `src/app.c` — SDK entrypoint and hardware adapter: USB CDC RX/TX, task/ring management, GPIO/ADC HAL, heater timer, emergency-stop fast path, and step timer.
- `src/pb_core.h` — portable protocol core: TBP frame validation/CRC, session sequencing, command queue/executor, status and event frames, sensors, safety gates, and watchdog/link-timeout behavior.
- `src/pb_printer.h` — shared hardware primitives for the TBP executor: GPIO pin writes, heater-off handling, motor phase drive and nonblocking step scheduling. It contains no wire-command parser.
- `src/pb_sensors.h` — temperature, battery and paper sensing/validation.
- `src/pb_tuning.h` — owner-retunable thermal, timing, motion and safety limits.
- `src/pb_pins.h` — generated pin capabilities from `pins.toml`.
- `src/pb_thermistor_table.h` — generated thermistor lookup table.
- `tests/c/` and `tests/transcripts/` — portable-core simulation and hex-frame golden transcripts.
- `tools/` — pin/thermistor code generators.

## Protocol

Every non-empty input line is interpreted as a TBP hex frame. Frames are newline terminated and contain `ID OP LEN DATA CRC16`, encoded as hexadecimal bytes. CR is accepted only as the final byte before LF. Any plaintext or malformed frame receives a TBP `ERR` frame; there is no text-command fallback. Replies and unsolicited events are also hex frames.

See [`../../../docs/thermal-protocol-spec.md`](../../../docs/thermal-protocol-spec.md) for the current command set and frame layout. The removed text commands and their replacements are recorded in [`../../../docs/legacy-text-protocol-removal.md`](../../../docs/legacy-text-protocol-removal.md).

## Build and safety

Firmware builds run through [the Pinbridge GitHub Actions workflow](../../../.github/workflows/pinbridge.yml). This environment has no local JieLi toolchain; do not attempt to reinstall it. CI builds do not flash hardware. **Do not flash or connect the printhead/motor/battery supply without a separate reviewed build and hardware-safety checklist.** This experimental firmware has not been validated on hardware for all timing, ADC, heater and disconnect cases.

Run the portable transcript tests with the repository's Python test suite or compile `tests/c/pb_test.c` against the synthetic and real pin tables. Do not run `cargo test` in this environment.
