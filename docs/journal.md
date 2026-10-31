# Journal

Research log.

- 2026-10-29: Initial workspace analysis. Determined the original payload is 512 KiB, but Soundbox SDK is ~790 KB (too big).
- 2026-10-29: Selected official JieLi Bluetooth SDK (`fw-AC63_BT_SDK`, BR23-family) as the base.
- 2026-10-29: Found the build host (`karna`), set up toolchain and post-build helper.
- 2026-10-29: Built blink-pc7 firmware for BR23, patched into dump.bin using `patch_app.py`.
- 2026-10-29: Flashed the blink firmware via `jluboottool.py`.
- 2026-10-29: Verified PC7 blinks as expected.

## Open tasks

- Identify the printer's actual pin map (print head, motor, paper sensor, button, battery sense).
- Add UART or BLE transport app.
- Investigate oscillator/power pins: why does the demo board config boot, but might differ?
- Make an automated build test that confirms reproducibility (the `faketime` fix).
- Archive the research phase (done).
- Cleanup: delete the Soundbox SDK checkout (it is unused) and the `work/` scratch directories.
