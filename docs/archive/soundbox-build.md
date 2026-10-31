# Soundbox SDK build system

## Checkout and build copy

Upstream checkout: `sdk/AC695N_Soundbox_SDK` (GitCode repository `yunthinker/AC695N_Soundbox_SDK`). The isolated working trees used for experiments are:

- `work/AC695N_Soundbox_SDK-stock` — stock 1 MiB configuration.
- `work/AC695N_Soundbox_SDK-AC6956C-512k` — copy with the selected smartbox board's flash macro changed to 512 KiB.

The upstream checkout should stay clean. Compare `git status` and diffs before/after a build. A working copy is disposable; save intentional changes as a patch or documented commit.

## Entry points

- Root `Makefile`: compiler variables, global defines, include paths, source list, object rules, linker invocation, and post-build hook.
- Board selection: `apps/soundbox/board/br23/board_ac695x_smartbox/board_ac695x_smartbox.h` for the 512 KiB experiment. This header contains `CONFIG_FLASH_SIZE` and `CONFIG_DOUBLE_BANK_ENABLE`.
- Linker script: `cpu/br23/sdk.ld` (and included section scripts such as `include_lib/system/system_lib_text.ld`, `system_lib_data.ld`, update and Bluetooth scripts). This sets code/RAM/PSRAM regions and collects sections.
- Post-build: `cpu/br23/tools/download.sh`, with `download.c` used by the SDK's packaging/build helper flow.

## Toolchain contract

The Linux Makefile expects `/opt/jieli/pi32v2/bin` with `clang`, `clang++`, `lto-wrapper`, `lto-ar`, `objcopy`, `objdump`, and `objsizedump`, plus sibling `lib/r3` and `include` directories. This workspace has a downloaded JieLi toolchain at `toolchain/jieli-linux-toolchains-20260730.1/`; the 512 KiB build explicitly overrode `TOOL_DIR`, `CC`, `CXX`, `LD`, and `AR` to use that tree rather than `/opt/jieli`.

Builds use `-target pi32v2 -mcpu=r3 -flto -Oz -g`, integrated assembler, and vendor-specific C compatibility flags. `-Oz` optimizes for size; `-g` keeps debug info in the ELF. `-flto` delays some optimization/code generation until link time. The exact defines are important: the root Makefile defines soundbox, audio, media, Bluetooth, update, filesystem and RTOS features. This is one reason a generic Soundbox build is not a suitable printer image without feature and board changes.

## Reproducible command used for the 512 KiB-configured compile

Run from the isolated working copy; paths below are relative to `$HOME/projects/thermal-printer`:

```bash
ROOT="$HOME/projects/thermal-printer"
TC="$ROOT/toolchain/jieli-linux-toolchains-20260730.1/pi32v2/bin"
cd "$ROOT/work/AC695N_Soundbox_SDK-AC6956C-512k"
ulimit -n 8192
make -j4 TOOL_DIR="$TC" CC="$TC/clang" CXX="$TC/clang++" \
  LD="$TC/lto-wrapper" AR="$TC/lto-ar" RUN_POST_SCRIPT=true all
```

The run exited 0 and linked `cpu/br23/tools/sdk.elf` (5,682,108 bytes) and `sdk.map` (~397 KiB). `RUN_POST_SCRIPT=true` was supplied as a Make variable during that experiment; it is **not** proof that the real firmware packaging flow ran. The old package files were still present with earlier timestamps. Do not reuse them as outputs of that build.

## Typical Make stages

1. Resolve target board, Make variables, feature defines and include directories.
2. Compile C/C++/assembly source to per-source intermediate objects in `objs/` (with LTO enabled).
3. Build/consume static archives and vendor libraries; some libraries are prebuilt and cannot be recompiled from this source tree.
4. Link the objects and archives through `lto-wrapper` using `cpu/br23/sdk.ld` and included linker fragments.
5. Emit `sdk.elf` and `sdk.map`; inspect link diagnostics, memory regions, section sizes and symbols.
6. Run post-build scripts to derive application sections, config, update/download packages, and possibly call helper utilities. The exact output depends on macros and host tools.
7. Validate the final artifact format, length, target metadata, flash offsets, checksums, and updater compatibility before any programming.

## Observed warning

The 512 KiB build logged an LLVM gold-plugin warning that a prebuilt module in `lib_cpu.a` had target triple `pi32` while the current link used `pi32v2`. Linking still exited 0, but this is a compatibility risk to investigate. A successful link alone cannot establish ABI correctness.

## Section packaging is not a substitute for the vendor packager

The Soundbox helper code concatenates sections in a specific order (`text.bin`, `data.bin`, `data_code.bin`, and optional codec/overlay payloads such as AEC, WAV, APE, FLAC, M4A, AMR, DTS, FM, MP3, WMA, and bank). Some sections are conditionally omitted or trimmed. Exact section names and trim rules must be read from the current SDK's `download.sh`/`download.c` and matching utility implementation. Do not guess section names or concatenate files by hand and call the result flashable. The generated `app.bin` is not necessarily the complete download package.
