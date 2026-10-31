# BR23 Transporter

RP2040 firmware plus a host tool for dumping and flashing the JieLi **BR23-family** thermal printer (chip marked AC6956C, board `JL BP21568-56C4`, 512 KiB SPI NOR) over its USB UBOOT. It is the FM-1 Transporter by kurogedelic (MIT, upstream `a7fb5e8`) retargeted to the printer, with a flasher for freshly built `app.bin` files.

```text
Mac ──USB (CDC 0 log, CDC 1 data)──► Pico/XIAO RP2040 ──D+/D-──► printer
 host/br23t.py                        firmware/  (USB_KEY entry + PIO USB host + UBOOT protocol)
```

**Status (2026-10-31)**
- Firmware builds (`build/br23/fm1_transporter.uf2`). Not yet flashed to a Pico or run on the printer in this form.
- Host tool: 14 tests pass against a fake Transporter (`tests/`). Not yet run against hardware.
- Already worked on hardware with an earlier hand-patched build (loader address and size only): `info` returned `key=3496 type=3 id=856013` and full dumps worked.

## Layout

| Path | What |
|---|---|
| `firmware/` | RP2040 firmware (C, Pico SDK). `src/jieli_uboot.h` holds the printer profile. |
| `firmware/lib/` | Vendored Pico-PIO-USB (`fe9133f`) and the two patches the build applies to a copy. |
| `host/br23t.py` | Host client: status, info, dump, flash, restore, selftest-write, ramrun. |
| `host/patch_app.py`, `host/jl.py` | Builds the flash image from `app.bin` (CRCs, re-scramble); pure Python, no crcmod. |
| `tests/` | `unittest` suite and a fake Transporter. |
| `.deps/`, `build/` | Pico SDK checkout and build output. Git-ignored. |

Not in this repo (git-ignored or never copied): the printer's `dump.bin`, the loader `br23loader.bin`, your `app.bin` builds.

## Setup

**Host** (Python 3.9+, pyserial):

```bash
uv run host/br23t.py --help        # or: pip install pyserial && python3 host/br23t.py --help
```

**Loader.** The Pico needs `br23loader.bin` from jl-uboot-tool: `data/loaderblobs/usb/br23loader.bin`, **11264 bytes**. It is *not* the 24576-byte `br23loader.bin` in the JieLi SDK; that one is refused. Pass `--loader PATH` or set `BR23T_LOADER`. The Pico keeps it in RAM until it is unplugged. Do not commit it.

**Firmware** (needs `cmake`, `arm-none-eabi-gcc`, and a Pico SDK; one is already in `.deps/`):

```bash
cmake -S firmware -B build/br23 -DPICO_SDK_PATH=$PWD/.deps/pico-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build/br23 -j2
```

Load `build/br23/fm1_transporter.uf2` (hold BOOTSEL, copy it over). Pins are unchanged from upstream: XIAO RP2040 GP0 = D+, GP1 = D-, GND; no VBUS to the printer. `-DFM1T_BOARD=debugprobe` is the Debug Probe variant.

## Using it

The Pico sends the USB_KEY while the printer powers on, so commands that need UBOOT wait for you to **switch the printer ON** (`--wait SEC`, default 60). If the printer was already on, switch it off and on again; `rekey` restarts the Pico's key.

First time, in this order (everything that writes is a dry run without `--write`):

```bash
br23t.py --loader L info                         # expect: key=3496 type=3 id=856013
br23t.py --loader L dump fresh.bin               # 512 KiB; --compare dump.bin to check against the original
br23t.py --loader L selftest-write --ref dump.bin --sector 0x50000 --write   # rewrites a sector with its own bytes
br23t.py plan  --app app.bin --dump dump.bin     # no hardware: build + self-check the image
br23t.py --loader L flash --app app.bin --dump dump.bin                      # dry run
br23t.py --loader L flash --app app.bin --dump dump.bin --write --run
br23t.py --loader L restore --dump dump.bin --write --run                    # factory app back
```

`--dump` is always the **original** dump, never a dump of an already-patched flash.

## Safety

Three layers, each refusing on its own:

1. **Pico firmware**: whole 4 KiB sectors in `[0x3000, 0x63000)` only, and only after the chip reports key `3496` / flash id `856013` (`ERR identity` otherwise). No block or chip erase, no chip-key write.
2. **`br23t`**: builds and re-parses the image (`patch_app.verify`), aborts if it changes anything outside the window, compares the flash against `--dump` first, writes only differing sectors, reads the whole flash back afterwards.
3. **Ordering**: sector `0x3000` is written last.

Why that sector matters: it is shared. It holds the last bytes of `uboot.boot`, `isd_config.ini`, the app header and entry list, and the start of `app.bin`, so every app flash rewrites it. A power loss during that one erase could damage the end of `uboot.boot` and the config. Recovery should work through the ROM key entry, but that has not been tested on a damaged chip. Do not unplug mid-write.

A flash rewrites about 96 sectors (the whole app area is re-padded and re-scrambled), not just the ones the new code touches.

Never run `burnchipkey`; it is one-way. Nothing here sends it.

## Gotchas

- `ERR loader` / "no br23loader": upload it with `--loader`.
- `ERR identity`: the chip is not the printer. Do not work around it.
- The Pico's data port is the *second* CDC. `br23t` finds it by sending `ping`; use `--port` if probing picks wrong.
- Upstream measured about 330 KiB/s for reads on the FM-1; sector writes are slower. Not timed on the printer yet.
- `firmware/docs/` was written for the FM-1; `PROTOCOL.md` is updated for the printer, `ARCHITECTURE.md` mostly applies.
- A few firmware comments and log lines still say FM-1 (`main.c`, `recovery.h`, `pio_host.c`); cosmetic.
- The FM-1 "soft key" code (stock V15 to UBOOT over USB-MIDI) is still in the firmware but cannot trigger on the printer. It could be removed later.

## What changed from upstream

- Printer profile in `firmware/src/jieli_uboot.h`: key `3496`, flash id `856013`, 512 KiB, write window `[0x3000, 0x63000)` (was FM-1: `980F`, `856014`, 1 MiB, `[0x4000, 0x93000)`).
- Loader at `0x12000`, 11264 bytes (was `0x1C02000`, 24064).
- Writes require a confirmed chip identity; BR23 UBOOT accepted by product id `2342`.
- Strings and log text say printer, not FM-1.
- Host: `fm1t.py` became `br23t.py`; the FM-1 `.fwsc` package flow was dropped; `flash`, `restore`, `plan` added; `--wait` for the power-on step.
- `crcmod` dependency removed (`host/jl.py`).
