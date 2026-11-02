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

- 2026-11-02: Follow-up pinbridge work added five transcript files (including real-table GET/STA no-argument checks), a Python unittest harness for C transcripts and fake subset, Board behavior tests, a capability-filtered optional Tk GUI, danger-gate pure logic, and keepalive HEL thread. The C runner reply consumption was corrected to preserve buffered subsequent lines. PTY simulator and full independent fake parity remain unfinished. GUI widgets were not visually exercised (headless environment).
- 2026-11-02: Updated Pinbridge `PSTEP` to the statically recovered official eight-state half-step sequence on `(PA9, PA10, PC3, PC2)`, with reverse traversing the sequence in descending order. Both WebSocket stepper examples now use the MCU-timed `action="printer"` step facade instead of sending per-phase GPIO changes over WebSocket; they express rate as a requested half-step period, default to a 6 ms period only as a comparison point, and explain that the firmware's 1000-iteration delay / nominal 6 ms event-post cadence are not calibrated motor timing. Added transcript checks for forward phase 0 and reverse phase 7. Python unittest suite: 32 passed; both demos compile and CLI help works; `git diff --check` passed. Isolated SDK build succeeded at `build-stepper8-20261102/app.bin` (68,556 bytes, SHA256 `9163179468b1e53ee1e88d66ef84ba8a52edb230d4f6d9f2b9f2faa2b7bc7e04`). Not flashed, published, or hardware-tested.
- 2026-11-02: Replaced the duplicated WebSocket stepper demos with one clean `host/pinbridge/examples/stepper_ws_demo.py` for the MCU-side `action=printer, confirm=printer, op=step` protocol; removed `stepper_ws_half_demo.py`. The single CLI supports `--steps` (half-step transitions) and `--full-steps` (two transitions per requested full-step angle), direction, requested rate/dwell, dry-run, and interactive confirmation. Host tests: 32 passed; Python compile/help and `git diff --check` passed. No hardware interaction or flashing performed.

## Public build publication

The current stepper8 build is published in the shared public folder as `pinbridge-stepper8-ce5edf24.bin`, with a matching `.sha256` file and build-info metadata. For future Pinbridge builds, publish every successfully completed build to `~/public/` automatically with a versioned, unambiguous filename, checksum, and build-info file; never publish failed/incomplete builds or secrets. Record publication in the project journal. Publication is not a flash or hardware test.
- 2026-11-02: Reworked Pinbridge `PSTEP` to run asynchronously on a 1 ms high-priority timer instead of a blocking busy-wait loop. USB command processing and software-watchdog polling continue during motion; added `PSTEPSTAT`, completion reporting, rounded-duration validation, and WebSocket polling until motion completes. Expanded the stepper demo limits to 60,000 transitions / 60 s while retaining 1 ms minimum timing resolution. Validation: 33 Python tests passed, including C golden transcript tests; Python compile/help checks and `git diff --check` passed. Isolated BR23 SDK build succeeded (`app.bin`, 69,276 bytes, SHA256 `8b5f99372892dfba8939d9e2fbefd8f9e337f6fd7f73ead975482febbb6864b4`). Published as `pinbridge-stepper-async-20261102.bin` with checksum and build metadata. Not flashed or hardware-tested; physical watchdog behavior and motor timing remain unverified.
