# Build experiment record

Date: 2026-10-29 (workspace timestamps). This is a snapshot of observed results; rerun checks when source, toolchain or board settings change.

## Original device evidence

- Board marking: `JL BP21568-56C4`.
- Working chip identification: AC6956C, BR23-family (not fully confirmed against official compatibility data).
- Flash ID: `0x856013`.
- Chip key: `0x3496`.
- Original entry reported from dump analysis: `0x01E00120`.
- Original dump: `dump.bin`, 524,288 bytes, SHA-256 `b46ea9a96d6b603b472c7d4551f958e708be3b36375a50314b8f9aeea89de32c`.

## Soundbox stock 1 MiB build

- Working copy: `work/AC695N_Soundbox_SDK-stock`.
- Configuration: stock `CONFIG_FLASH_SIZE (1024 * 1024)`, double bank disabled.
- ELF: `artifacts/stock-1mb-build/decompile/` has disassembly, symbols, section listing, sizedump, linker map and resolution output.
- Raw application payload extracted from the ELF: `artifacts/stock-1mb-build/stock-1mb-app.bin`, 792,762 bytes, SHA-256 `054bb0d4ef6189cfd5bd7e8c6b73f8f442740aafe0adb3eac529f71669d8386e`.
- It is not a whole-chip flash image and is not considered a candidate image for the 512 KiB target.

## Soundbox 512 KiB-configured compile

- Working copy: `work/AC695N_Soundbox_SDK-AC6956C-512k`.
- Local config change: `apps/soundbox/board/br23/board_ac695x_smartbox/board_ac695x_smartbox.h` changed `CONFIG_FLASH_SIZE` from 1 MiB to 512 KiB; `CONFIG_DOUBLE_BANK_ENABLE` remained `0`.
- Toolchain: `toolchain/jieli-linux-toolchains-20260730.1/pi32v2/bin` plus sibling common/lib/include files.
- Build command and arguments are recorded in [Soundbox SDK build pipeline](soundbox-build.md).
- Exit marker `/tmp/ac6956c-512k-build.exit` contained `0`; log `/tmp/ac6956c-512k-build.log` ended after link/post-build.
- Fresh `sdk.elf`: 5,682,108 bytes; `sdk.map`: ~397 KiB; generated around 15:27 on 2026-10-29.
- Warning: prebuilt `lib_cpu.a` module target triple `pi32` vs current `pi32v2`.
- Package outputs `app.bin`, `jl_isd.bin`, `.fw`, `.ufw` retained older 14:43 timestamps and were not proven to be fresh outputs from this run.
- A fresh application payload was separately extracted from the ELF to `artifacts/ac6956c-512k-build/app.bin`: 792,762 bytes. SHA-256 is listed in `artifacts/ac6956c-512k-build/SHA256SUMS`. This is over the total 512 KiB capacity and **not flashable**.

## Official Bluetooth SDK

- Repository: `sdk/fw-AC63_BT_SDK`, commit `fdc018d`, tag `ac63_patch_release_20250224`.
- Available example targets include `ac635n_hid` and `ac635n_spp_and_le` (BR23 board paths).
- Linux Makefiles expect `pi32v2` toolchain layout and `lib/r3`/`include` siblings. The workspace has the expected named tools.
- Official Bluetooth SDK test-build and package generation remain unverified at this snapshot.

## Conclusions / next work

1. Prefer a minimal Bluetooth transport app over Soundbox; choose SPP/BLE/HID based on the printer's original protocol and intended host support.
2. Test-build official SDK in a disposable copy, first without changing upstream.
3. Confirm AC6956C-specific startup/ROM/library compatibility; inspect target-triple warnings.
4. Configure all application, linker, and post-build flash-size values consistently; reduce features until loadable regions fit 512 KiB.
5. Trace the official packager and validate package metadata and layout. Do not reuse old `.fw`/`.ufw` files.
6. Keep original dump untouched and do not program until the final image and recovery plan are verified.
