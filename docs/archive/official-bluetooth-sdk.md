# Official JieLi Bluetooth SDK

## Repository and targets

Repository: `https://github.com/Jieli-Tech/fw-AC63_BT_SDK` (local checkout `sdk/fw-AC63_BT_SDK`). The local checkout was at commit `fdc018d`, tag `ac63_patch_release_20250224`. A second checkout `sdk/fw-AC63_BT_SDK_audio-example` exists for the related audio example. Preserve the upstream repositories clean and make board changes only in a separate working copy.

The root Makefile exposes BR23-family targets including:

- `make ac635n_hid` → `apps/hid/board/br23`
- `make ac635n_spp_and_le` → `apps/spp_and_le/board/br23`

Other AC63 targets select different board families (BR25, BR30, BR34, BD19, BD29). Do not choose a target based solely on a similar product name; target selection selects startup, libraries, drivers and board configuration.

## Linux toolchain

The BR23 Makefiles expect `/opt/jieli/pi32v2/bin` and sibling `lib/r3` and `include`. They set `-target pi32v2 -mcpu=r3 -flto -Oz -g`, use `clang`, `lto-wrapper`, and `lto-ar`, and export `objdump`, `objcopy`, and `objsizedump`. The existing workspace toolchain appears structurally suitable for testing this Makefile, but the official SDK build has **not yet been run and validated** as part of the recorded experiment. Use a working copy and explicit tool paths; don't alter `/opt/jieli` globally without a reason.

## Flash-size configuration

The BR23 HID/SPP examples include a board post-build configuration header with values such as `FLASH_SIZE_512K 0x80000` and `FLASH_SIZE_1M 0x100000`, but the checked-in example defaults to `CONFIG_FLASH_SIZE FLASH_SIZE_1M`. A 512 KiB build requires consistent configuration in the application/board headers, post-build config, linker memory regions and packaging metadata. Changing just one macro is not enough.

## What this SDK does and doesn't prove

It is the more relevant starting point for Bluetooth printer transports than a full Soundbox application because it has HID and SPP/BLE examples. Still, its named `ac635n` targets and libraries are not evidence of confirmed AC6956C compatibility. Check chip IDs, ROM API assumptions, entry/startup address, interrupt table, flash ID and offsets, oscillator/RF config, UART/USB pins, and package chip/PID metadata against the original dump and board. Bluetooth HID is not USB HID. No official-SDK test image has been approved or flashed in this project.

## Suggested test sequence

1. Create an isolated copy of `sdk/fw-AC63_BT_SDK` and record commit/diff.
2. Point only that build at the local toolchain paths; run `make -n ac635n_spp_and_le` and inspect all commands/post-build hooks before running.
3. Build the simplest likely transport example; capture full log and exit code.
4. Inspect ELF target, entry point, linker map, libraries and warnings. Compare memory regions and ROM stubs with the original image.
5. Configure 512 KiB in all relevant places, then inspect linker and post-build layouts. Reduce features deliberately if the application does not fit.
6. Run the official packager in a controlled environment; confirm generated package format and metadata from SDK source/tool help. Never run a command named `download`/`isd_download` against a device unless intentionally entering a verified programming operation.
7. Do not flash until a recovery plan, full original dump, exact flash layout and chip compatibility are confirmed.
