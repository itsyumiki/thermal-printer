# SAFETY — READ BEFORE POWERING THE BOARD

The printer pin map is only partly known. Raw pin control can burn the thermal head or overheat motor coils. **Test on USB power only, with the thermal head disconnected and motor/battery supply off.** Never experiment on the assembled printer with power circuitry attached. Stepper and printhead pins are intentionally host-accessible for driver development; PB11 heater-strobe control is especially hazardous. GPIO access does not provide deterministic pulse timing over USB CDC.

Boot does not intentionally configure GPIO; only SDK startup behavior remains. No protocol pin is touched until `MOD`. Pins absent from the allow-list return `err pin`. The watchdog defaults to 5000 ms and returns active outputs to high impedance after a timeout. This code has **not been tested on hardware**.

# pinbridge

An experimental jlpin v2 text pin server for JieLi AC6956C / AC635N (BR23), over USB CDC. `src/app.c` owns the SDK-facing GPIO/ADC and USB tasks. `src/pb_core.h` owns wire framing, reply construction, shared state and watchdog polling; its internal modules `src/pb_commands.h` and `src/pb_printer.h` handle GPIO/ADC commands and the printer/stepper respectively. The real pin table `src/pb_pins.h` is generated from `pins.toml`.

## Build (does not flash)

From the repo root, run `nice -n 19 ~/projects/scratch-bridge/tools/mk2.sh firmware/apps/pinbridge pinbridge`. The helper builds in a fresh patched SDK copy. Flashing is deliberately not run here. For the owner-controlled flash flow, first review `docs/flashing.md` and `docs/hardware-and-flash.md`, then follow that repository's documented image-patching and flashing steps only after reviewing the build output and taking the safety precautions above.

The [Pinbridge C firmware GitHub Actions workflow](../../../.github/workflows/pinbridge.yml) builds on pushes to `pinbridge` and pull requests targeting it. It runs the existing `scripts/build-app.sh`, caches the pinned vendor archives and SDK Git mirror, and uploads `app.bin`, `sdk.elf`, `sdk.map`, checksums and build logs. CI does not run tests or flash hardware. Handwritten C code uses the adjacent `.clang-format` rules; `src/pb_pins.h` is generated from `pins.toml` and must not be formatted manually.

## Two-minute REPL try-out

Install the host package in a venv: `python -m pip install ./host/pinbridge` (or `uv pip install ./host/pinbridge`). Once the owner has flashed a reviewed build and confirmed the safe test setup, run `PYTHONPATH=host/pinbridge python -m pinbridge repl --port /dev/ttyACM0`. Try `HEL`, `STA`, `MOD PC7 lo`, `SET PC7=1`, `GET PC7`, `RST`. Use the explicit `--port`; USB VID/PID auto-detection is not implemented.

## Pin table

See generated [`pins_table.md`](pins_table.md). Current config hash is generated in that table and returned by `HEL`. Allowed pins include PC7 (digital GPIO, `led` alias, known-good LED test net); PA9/PA10/PC2/PC3 (stepper phases); PB8/PB9/PB10/PB11 (printhead latch/clock/data/heater strobe); and PB1/PB3/PC4/PC5 sensor/power-sense pins. ADC mode is exposed for the two analog sensor nets: PC4 (SDK channel 10) and PC5 (SDK channel 13); PB1 (channel 5) and PB3 (channel 6) are ADC-capable in the SDK but remain digital-only in Pinbridge so the printer status loop can reliably track the button and paper sensor. PWM remains out of scope. The printer status loop keeps PB1/PB3 in their measured digital modes and samples PC4/PC5 as analog inputs. The added stepper/head assignments are based on owner notes and are not hardware-verified.

## ADC implementation

The BR23 SDK provides `adc_add_sample_ch(ch)` to register a GPIO ADC channel with its periodic scanner, `adc_get_value(ch)` for the latest raw count, and `adc_get_voltage(ch)` for the SDK-calibrated signal voltage. The pinbridge HAL registers a channel on first use, allows the SDK scanner 100 ms to populate it, and then returns `raw/mV` from `ADC PIN`. `ADC vbat` uses the SDK's built-in quarter-battery channel and scales the reported voltage by four. PC5 is a separate board voltage-divider net, so its measured pin voltage is not converted to battery voltage without a confirmed divider ratio. ADC reads are asynchronous/stale until the SDK scanner has collected a sample; ADC, GPIO mode switching, and voltage conversion remain untested on hardware.

Enabled: PA0 and PA1 are now included in the firmware GPIO allow-list; PA1 output changes may affect board power and require explicit UI confirmation. Denied: PA3 (not on package), PB5 (charger/LDO input), DP/DM (USB data). Other unlisted pins remain denied until bonding and electrical ownership are confirmed. Driver pins are not denied merely because they belong to printer subsystems.

## Build size [compiled]

Latest local build (`~/projects/scratch-bridge/build-pinbridge`): `size sdk.elf` reports text 67,120 B, data 7,952 B, bss 10,148 B; `app.bin` 67,160 B, below the 393,216 B application area (0x3000..0x63000) by 326,056 B. Against `build-basecdc/app.bin` (81,440 B from the baseline build record), this is 14,280 B smaller. SHA-256: `2c6b663dd54e7ea11476c40a3596fe29b7772619f57538b27f709d6f34212d1c`. The same image is published at `https://mcp.yumiki.me/shared/pinbridge-app.bin` after successful Pinbridge builds. These are [compiled] artifacts only; not proof of runtime safety.

The SDK symbol-resolution report resolves the app overrides of `log_print`, `printf`, `puts`, `putchar`, and `put_buf` to `app_idle.o` [compiled]. The resolution file still shows `task_pc.c.o` referencing `cdc_write_data` in the stock `usb_cdc_wakeup`; replacing the wakeup callback at runtime is required and remains [untested on hardware].

## Web UI and WebSocket backend

Install the host package (the WebSocket/HTTP server is included): `python -m pip install -e ./host/pinbridge`.

Start the browser UI with `PYTHONPATH=host/pinbridge python -m pinbridge webui`. Pinbridge starts its HTTP + WebSocket backend on `http://127.0.0.1:8765` and opens the default browser automatically. Choose a serial port in the page, then click **Connect**. To open directly with a selected device, use `python -m pinbridge webui --port /dev/ttyACM0`. Use `python -m pinbridge webui --mock` to explore the interface without hardware, `--http-port 9000` to change the port, or `--no-browser` to suppress automatic browser launch. The legacy `gui` command now launches the same Web UI.

The Python backend serves the bundled HTML, CSS, JavaScript and JSON/WebSocket API. The browser and backend communicate over one WebSocket connection for connection state, port discovery, GPIO mode changes, reads/writes, reset-to-safe-state, raw protocol commands and live device events. A one-second printer poll translates PB3 into paper-present/empty, PB1 into power-button pressed/released, PA1 into the power-latch state, and reads raw/mV ADC values from PC4 and PC5. The UI includes a translated printer-status island above the raw pin matrix, searchable pin cards, per-port filtering, live status, output-level controls, reserved-pin explanations, confirmation prompts for subsystem outputs, and a diagnostics console. On a successful connection, a startup macro configures PA1 as output HIGH to latch printer power on, then configures PB1 with pull-up for power-button sensing; a toast confirms that the physical power button can be released. The dedicated Power button toggles PA1 HIGH/LOW. First-click Read automatically configures safe, non-power/non-driver pins as inputs; it never silently reconfigures PA1, stepper, or print-head pins. Static assets are bundled with the Python package and do not require a separate frontend build or external font/CDN request.

By default the server binds to localhost only. Do not expose it to an untrusted network: it is a hardware-control interface and currently has no authentication. The UI only offers the capabilities in the generated firmware pin table; PA3, PB5, DP and DM remain unavailable as GPIO. Mock mode exercises UI flows but is not a safety or timing test. Do not connect the print head, motor or battery supply during initial GPIO experiments. No device is flashed by starting the Web UI.

## Limits and status

- [assumed] Pin bonding and net assignments come from the owner's documentation; no hardware is available here.
- [untested on hardware] CDC wakeup replacement, detach safe state, GPIO register sequencing, ADC channel mux/scanner interaction, voltage accuracy, watchdog timing and USB behavior.
- [compiled] is only claimed for build stages that complete; check the final project report for exact result.
- The 10 ms OS tick limits watchdog/listener granularity. The SDK's built-in battery channel is stale/averaged by its slow refresh loop; GPIO ADC channels use the SDK's periodic scanner. `GET` on input modes (`in`, `up`, `dn`) samples the GPIO pad input register; `GET` on outputs (`lo`, `hi`) reports the software shadow, not verified pad readback.
- PWM is intentionally not exposed on the real pin table. DTR state is not readable in the SDK; unplug/detach event handling is the available best-effort safe-state mechanism. A closed host port relies on the watchdog.
- The portable core currently has a basic implementation; do not treat this experimental branch as production-safe until all protocol and firmware limitations are resolved and the hardware checklist is completed.

## Owner hardware checklist (only after a reviewed build)

1. USB power only; head disconnected; motor/battery supply off. Keep a recovery/known-good firmware image ready.
2. Enumerate CDC and send `HEL`; report exact line/hash and whether `STA` responds cleanly without boot/log noise.
3. Send `MOD PC7 lo`, `SET PC7=1`, `SET PC7=0`, `RST`; report LED behavior and any resets.
4. With a meter only and the same safe setup, verify PB1 pull-up (released HIGH / pressed LOW), PB3 without pulls (paper present LOW / empty HIGH), and compare `ADC PC4 PC5` raw/mV readings against the measured signal voltages. Do not infer printhead temperature or battery voltage until the thermistor curve and divider ratio are confirmed.
5. Arm an output, stop sending commands, and verify `!wdg` plus safe state after about five seconds. Unplug USB while output is configured and report safe-state behavior.
6. Do not connect the print head/motor/battery supply during initial GPIO tests. PA1 output may affect board power; PB5 is the charger/LDO input, and DP/DM are USB data lines. Test the newly allowed stepper/head pins first as unloaded GPIO and only with the owner-reviewed pin map. Send the full transcript, board revision, firmware hash and any unexpected lines to the maintainer.


## Experimental low-level printer commands

The portable command core includes MCU-timed experimental commands, also exposed by the WebSocket `action: "printer"` facade (`confirm: "printer"`):

- `PHEAD <96 hex chars> <clock-half-period-us> <data-setup-us>` — shifts 48 bytes, LSB-first. Requires PB8–PB11 configured as outputs. Timing range policy is host-side; firmware accepts any unsigned 32-bit timing value.
- `PLATCH <pulse-us>` — PB8 low pulse then high. Timing range policy is host-side.
- `PHEAT <pulse-us>` — one PB11 high pulse then low. Timing range policy is host-side; this is **not** the stock timer ISR and there is no firmware-side thermal safety cutoff.
- `PSTEP <FWD|REV> <count> <inter-step-us> <coil-on-us>` — official eight-state half-step sequence on PA9/PA10/PC3/PC2. Runs asynchronously on a 1 ms high-priority timer so USB command handling and software watchdog polling continue while the motor moves. Use `PSTEPSTAT` to query `ok busy` / `ok done`. No firmware-side count or total-duration cap; count and duration policy belongs to the host. The current 1 ms scheduler still rounds timing up and does not support sub-millisecond transitions. The half-step phase (0..7) is kept in firmware RAM and persists across PSTEP commands (reset to 0 on power-up/`pb_init`; not cleared by `PSTEP`, `MOD` or safe state): each command first re-asserts the remembered phase (this is the `count` initial phase), then FWD steps +1 and REV steps -1 per transition. Coil state remains at the last phase until changed/coasted.

  Static analysis found that the official firmware uses an eight-state sequence and a software loop with argument 1000 after each phase. Separately, its nominal 2 ms callback posts print/feed events at a minimum divider of 3 (nominally 6 ms between event posts). These are **not** a calibrated step period. Pinbridge now schedules steps asynchronously at 1 ms resolution; actual motor speed, torque limits, and wiring remain hardware-dependent and unverified.

These commands are experimental low-level primitives, not a complete printer scheduler. They do not implement the stock four-phase row sequence, temperature interlocks, paper detection, or native heater timing. Use the host example `host/pinbridge/examples/printhead_dotted_line_ws_demo.py`; by default it prints only one row and asks for typed confirmation. Source parser/WebSocket tests cover argument handling only. The isolated vendor-SDK firmware build completed successfully for this change; the image has not been flashed or hardware-tested. The busy-loop delay implementation is approximate and uncalibrated, so it does not provide exact microsecond timing or a certified heater safety limit. Review the build notes and hardware plan before any flash/run.
