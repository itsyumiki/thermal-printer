# SAFETY — READ BEFORE POWERING THE BOARD

The printer pin map is only partly known. Raw pin control can burn the thermal head or overheat motor coils. **Test on USB power only, with the thermal head disconnected and motor/battery supply off.** Never experiment on the assembled printer with power circuitry attached. Stepper and printhead pins are intentionally host-accessible for driver development; PB11 heater-strobe control is especially hazardous. GPIO access does not provide deterministic pulse timing over USB CDC.

Boot does not intentionally configure GPIO; only SDK startup behavior remains. No protocol pin is touched until `MOD`. Pins absent from the allow-list return `err pin`. The watchdog defaults to 5000 ms and returns active outputs to high impedance after a timeout. This code has **not been tested on hardware**.

# pinbridge

An experimental jlpin v2 text pin server for JieLi AC6956C / AC635N (BR23), over USB CDC. Firmware source is `src/app.c`; portable protocol core is `src/pb_core.h`; the generated real pin table is `src/pb_pins.h`.

## Build (does not flash)

From the repo root, run `nice -n 19 ~/projects/scratch-bridge/tools/mk2.sh firmware/apps/pinbridge pinbridge`. The helper builds in a fresh patched SDK copy. Flashing is deliberately not run here. For the owner-controlled flash flow, first review `docs/flashing.md` and `docs/hardware-and-flash.md`, then follow that repository's documented image-patching and flashing steps only after reviewing the build output and taking the safety precautions above.

## Two-minute REPL try-out

Install the host package in a venv: `python -m pip install ./host/pinbridge` (or `uv pip install ./host/pinbridge`). Once the owner has flashed a reviewed build and confirmed the safe test setup, run `PYTHONPATH=host/pinbridge python -m pinbridge repl --port /dev/ttyACM0`. Try `HEL`, `STA`, `MOD PC7 lo`, `SET PC7=1`, `GET PC7`, `RST`. Use the explicit `--port`; USB VID/PID auto-detection is not implemented.

## Pin table

See generated [`pins_table.md`](pins_table.md). Current config hash is generated in that table and returned by `HEL`. Allowed pins: PC7 (digital GPIO, `led` alias, known-good LED test net); PA9/PA10/PC2/PC3 (stepper phases); PB8/PB9/PB10/PB11 (printhead latch/clock/data/heater strobe); PB1/PB3/PC4/PC5 remain input-oriented sensor/power-sense pins. This configuration exposes only normal digital GPIO modes (input/output/pulls/high-impedance); ADC and PWM are out of scope for this phase. The added stepper/head assignments are based on owner notes and are not hardware-verified.

Denied: PA0 (SDK/log ownership and bonding unresolved), PA1 (power control), PA3 (not on package), PB5 (charger/LDO input), DP/DM (USB data). Other unlisted pins remain denied until bonding and electrical ownership are confirmed. Driver pins are not denied merely because they belong to printer subsystems.

## Build size [compiled]

Latest final local build (`~/projects/scratch-bridge/build-pinbridge-final2`): `size sdk.elf` reports text 66,776 B, data 7,952 B, bss 10,084 B; `app.bin` 66,816 B, below the 393,216 B application area (0x3000..0x63000) by 326,400 B. Against `build-basecdc/app.bin` (81,440 B from the baseline build record), this is 14,624 B smaller. SHA-256: `6611f3663248e2d1aba66178f5a3547f46603e0bfa6514f3ef3e526801def5da`. These are [compiled] artifacts only; not proof of runtime safety.

The SDK symbol-resolution report resolves the app overrides of `log_print`, `printf`, `puts`, `putchar`, and `put_buf` to `app_idle.o` [compiled]. The resolution file still shows `task_pc.c.o` referencing `cdc_write_data` in the stock `usb_cdc_wakeup`; replacing the wakeup callback at runtime is required and remains [untested on hardware].

## Optional GUI (`pinbridge-gui`)

Install the optional GUI extra in a virtual environment: `python -m pip install -e './host/pinbridge[gui]'`. Run `PYTHONPATH=host/pinbridge python -m pinbridge gui --mock` for a fake-only try-out, `PYTHONPATH=host/pinbridge python -m pinbridge gui --port /dev/ttyACM0` to connect immediately to an explicitly selected CDC port, or `PYTHONPATH=host/pinbridge python -m pinbridge gui` to open the desktop UI disconnected and choose a port there.

The compact dark desktop UI follows the current same-owner gpiodemo controller's main layout: a resizable GPIO/serial-log split, port picker and connect/disconnect controls, per-port pin tabs, bulk scope/mode actions, a live event/serial log, timestamp and autoscroll options, raw command entry with Up/Down history, pin map, handshake/config info, status, debug output, watchdog setting, and reset controls. Pin rows include a **Description** column. Every pin in the current documented map is shown, including denied/system entries such as PA0, PA1, PA3, PB5, DP and DM; those entries are marked RESERVED and have no GPIO controls. The current firmware allow-list remains the source of enabled capabilities. The actual MCU package pin map is not complete, so undocumented pins are not invented or presented as known physical pins.

This is a new Python/Tk implementation inspired by the same-owner AGPLv3 gpiodemo UI; its Rust wire protocol and SAM4E pin map are not reused. Pinbridge does not yet have the route/bank discovery protocol used by gpiodemo, so the GUI provides the fixed Pinbridge port tabs instead. Stepper/head outputs require confirmation; PB11 requires typing `PB11`. Startup does not configure pins, and reconnect/reset does not restore outputs. The large warning banner has been replaced by a compact safety reminder in the connection bar.

[tested on host] GUI map and safety-gate unit tests, adapter tests, and the complete host suite pass. [untested visually] No display/Xvfb is available, so widget geometry and interactions have not been visually exercised. [untested on hardware] Serial event timing and GUI behavior against firmware are not tested. The in-memory fake's listener/watchdog fidelity is limited; use `--mock` to exercise UI paths only, not to validate safety timing. The C `tests/c/pb_sim.c` simulator can be run over a POSIX PTY by the host test suite to exercise `Board` framing and command round trips against the real generated pin table; its simulated clock is byte-driven, not a watchdog timing model.

## Limits and status

- [assumed] Pin bonding and net assignments come from the owner's documentation; no hardware is available here.
- [untested on hardware] CDC wakeup replacement, detach safe state, GPIO register sequencing, ADC sampling, watchdog timing and USB behavior.
- [compiled] is only claimed for build stages that complete; check the final project report for exact result.
- The 10 ms OS tick limits watchdog/listener granularity. ADC battery readings in the SDK are stale/averaged by its slow refresh loop. `GET` on outputs reports the software shadow, not verified pad readback.
- PWM is intentionally not exposed on the real pin table. DTR state is not readable in the SDK; unplug/detach event handling is the available best-effort safe-state mechanism. A closed host port relies on the watchdog.
- The portable core currently has a basic implementation; do not treat this experimental branch as production-safe until all protocol and firmware limitations are resolved and the hardware checklist is completed.

## Owner hardware checklist (only after a reviewed build)

1. USB power only; head disconnected; motor/battery supply off. Keep a recovery/known-good firmware image ready.
2. Enumerate CDC and send `HEL`; report exact line/hash and whether `STA` responds cleanly without boot/log noise.
3. Send `MOD PC7 lo`, `SET PC7=1`, `SET PC7=0`, `RST`; report LED behavior and any resets.
4. With a meter only and the same safe setup, test PB1/PB3/PC4/PC5 as digital inputs (PB1 may use pull-up); ADC is out of scope for this build.
5. Arm an output, stop sending commands, and verify `!wdg` plus safe state after about five seconds. Unplug USB while output is configured and report safe-state behavior.
6. Do not connect the print head/motor/battery supply during initial GPIO tests. Do not drive PA1, PB5, DP or DM. Test the newly allowed stepper/head pins first as unloaded GPIO and only with the owner-reviewed pin map. Send the full transcript, board revision, firmware hash and any unexpected lines to the maintainer.
