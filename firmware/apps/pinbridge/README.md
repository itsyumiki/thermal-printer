# SAFETY — READ BEFORE POWERING THE BOARD

The printer pin map is only partly known. Raw pin control can burn the thermal head or overheat motor coils. **Test on USB power only, with the thermal head disconnected and motor/battery supply off.** Never experiment on the assembled printer with power circuitry attached.

Boot does not intentionally configure GPIO; only SDK startup behavior remains. No protocol pin is touched until `MOD`. Pins absent from the allow-list return `err pin`. The watchdog defaults to 5000 ms and returns active outputs to high impedance after a timeout. This code has **not been tested on hardware**.

# pinbridge

An experimental jlpin v2 text pin server for JieLi AC6956C / AC635N (BR23), over USB CDC. Firmware source is `src/app.c`; portable protocol core is `src/pb_core.h`; the generated real pin table is `src/pb_pins.h`.

## Build (does not flash)

From the repo root, run `nice -n 19 ~/projects/scratch-bridge/tools/mk2.sh firmware/apps/pinbridge pinbridge`. The helper builds in a fresh patched SDK copy. Flashing is deliberately not run here. For the owner-controlled flash flow, first review `docs/flashing.md` and `docs/hardware-and-flash.md`, then follow that repository's documented image-patching and flashing steps only after reviewing the build output and taking the safety precautions above.

## Two-minute REPL try-out

Install the host package in a venv: `python -m pip install ./host/pinbridge` (or `uv pip install ./host/pinbridge`). Once the owner has flashed a reviewed build and confirmed the safe test setup, run `PYTHONPATH=host/pinbridge python -m pinbridge repl --port /dev/ttyACM0`. Try `HEL`, `STA`, `MOD PC7 lo`, `SET PC7=1`, `GET PC7`, `RST`. Use the explicit `--port`; USB VID/PID auto-detection is not implemented.

## Pin table

See generated [`pins_table.md`](pins_table.md). Current config hash: `e611f647` (`HEL` hash). Allowed pins: PC7 (digital in/out and pulls; `led` alias, known-good LED test net), PB1 (input/pull-up/ADC5, `button`, sense only), PB3 (input/ADC6, `paper`, sense only), PC4 (input/ADC10, `head_temp`, sense only), PC5 (input/ADC13, `vbat_div`, sense only). Only PC7 permits output. No real configured pin supports PWM.

Denied: PA0 (UART0/log pad; bonding not confirmed), PA1 (power control), PA3 (not on package), PA9/PA10/PC2/PC3 (stepper phases), PB8/PB9/PB10 (head latch/clock/data), PB11 (heater strobe), PB5 (charger/LDO input), DP/DM (USB data). Every other unlisted pin is denied because the available evidence does not confirm safe bonded-out use. The source evidence is the owner's pin ledger and datasheet notes, not hardware verification by this project.

## Build size [compiled]

`build-pinbridge3/sdk.elf`: text 66,184 B, data 7,952 B, bss 9,856 B. `app.bin`: 66,224 B, below the 393,216 B application area (0x3000..0x63000) by 326,992 B. The existing hello-cdc baseline (`build-basecdc`) has text 81,400 B, data 7,952 B, bss 9,000 B and app.bin 81,440 B. Delta: text -15,216 B, data 0 B, bss +856 B, app.bin -15,216 B. These are build artifacts only; not proof of runtime safety.

The SDK symbol-resolution report resolves the app overrides of `log_print`, `printf`, `puts`, `putchar`, and `put_buf` to `app_idle.o` [compiled]. The resolution file still shows `task_pc.c.o` referencing `cdc_write_data` in the stock `usb_cdc_wakeup`; replacing the wakeup callback at runtime is required and remains [untested on hardware].

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
4. With a meter only and the same safe setup, test PB1/PB3/PC4/PC5 as inputs/ADC without connecting external loads; report raw/mV stability and whether values change with known-safe stimuli.
5. Arm an output, stop sending commands, and verify `!wdg` plus safe state after about five seconds. Unplug USB while output is configured and report safe-state behavior.
6. Do not test denied pins or connect the print head/motor/battery supply. Send the full transcript, board revision, firmware hash and any unexpected lines to the maintainer.
