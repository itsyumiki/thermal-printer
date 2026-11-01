# Journal

Research log.

- 2026-10-29: Initial workspace analysis. Determined the original payload is 512 KiB, but Soundbox SDK is ~790 KB (too big).
- 2026-10-29: Selected official JieLi Bluetooth SDK (`fw-AC63_BT_SDK`, BR23-family) as the base.
- 2026-10-29: Found the build host (`karna`), set up toolchain and post-build helper.
- 2026-10-29: Built blink-pc7 firmware for BR23, patched into dump.bin using `patch_app.py`.
- 2026-10-29: Flashed the blink firmware via `jluboottool.py`.
- 2026-10-29: Verified PC7 blinks as expected.
- 2026-11-01: Built `ble-uart-serial` (USB CDC <-> BLE bridge, shared core in `firmware/shared/uart-bridge/`), flashed it and tested on hardware: both directions work (MTU 185, name JL-UART-BLE). Open issues (ADKEY log spam on PB1, one unexplained reset, boot logs dropped, slave latency 10) are in `docs/uart-bridge-comparison.md`.
- 2026-11-01: Decided not to build the Classic SPP variant (BLE-only: cross-OS reach, the project protocol is BLE already). Rationale in `docs/uart-bridge-comparison.md`.

- 2026-11-01: Started pinbridge on branch `pinbridge`: generated pin tables/hash (`e611f647` initial config), initial header-only protocol core, Python host skeleton, firmware adapter and safety documentation. The owner-approved pin list now hashes to `546c66a8`; ADC/PWM remain deferred. C transcript runner now compares expected lines strictly and was verified to exit non-zero for a deliberately wrong reply. Current smoke suite remains deliberately partial (one transcript, three Python tests); this is not a hardware-ready release.
- 2026-11-01: Pinbridge validation in the handoff session: generated-file check passes; GCC strict C harness compiles with `-Wall -Wextra -Werror`; synthetic-table smoke transcript passes and deliberately wrong transcript exits 1; Python unittest suite has 3 passing tests. Firmware build status and final size are recorded in the handoff report. No hardware or flashing was available/performed.

## Open tasks

- Identify the printer's actual pin map (print head, motor, paper sensor, button, battery sense).
- BLE transport app: first version done (`ble-uart-serial`), follow-up fixes listed in `docs/uart-bridge-comparison.md`.
- Investigate oscillator/power pins: why does the demo board config boot, but might differ?
- Make an automated build test that confirms reproducibility (the `faketime` fix).
- Archive the research phase (done).
- Cleanup: delete the Soundbox SDK checkout (it is unused) and the `work/` scratch directories.
