# Build flow diagrams and command recipes

## Soundbox build flow

```text
Board header + Makefile feature defines
                 |
                 v
       C/C++/assembly sources
                 |
                 v
     clang / integrated assembler
       -target pi32v2 -mcpu=r3
          -Oz -g -flto
                 |
                 v
      intermediate objects / LLVM IR
                 |
                 +------ prebuilt JieLi .a libraries
                 |          (must match target/ABI)
                 v
  lto-wrapper + LLVM plugin + sdk.ld
                 |
                 +----> sdk.elf (linked ELF)
                 +----> sdk.map (memory/symbol map)
                 |
                 v
       post-build section extraction
                 |
                 v
   app payload + config + package tools
                 |
                 v
   ISP/download/OTA-specific container
                 |
                 v
  validate target/addresses/size/checksums
                 |
                 v
    only then consider programming device
```

## Official Bluetooth SDK flow

```text
root target (e.g. ac635n_spp_and_le / ac635n_hid)
                 |
                 v
 apps/<profile>/board/br23 Makefile + board config
                 |
                 v
 clang + pi32v2/r3 flags + profile sources
                 |
                 v
 vendor libraries + LTO + BR23 linker script
                 |
                 v
       ELF / map / profile package outputs
                 |
                 v
 verify AC6956C compatibility and 512 KiB map
```

The target labels above are examples from the official repository; they are not an assertion that the AC635N example is proven to run on the AC6956C printer.

## Useful inspection commands

Run these from a working copy, adapting `ROOT`/`TC` to the actual workspace:

```bash
# Verify upstream checkout and local changes
git -C "$ROOT/sdk/AC695N_Soundbox_SDK" status --short --branch
git -C "$ROOT/sdk/fw-AC63_BT_SDK" status --short --branch

# Inspect the linked ELF and its sections
"$ROOT/toolchain/jieli-linux-toolchains-20260730.1/common/bin/objdump" -h "$WORK/cpu/br23/tools/sdk.elf"
"$ROOT/toolchain/jieli-linux-toolchains-20260730.1/common/bin/objdump" -t "$WORK/cpu/br23/tools/sdk.elf"

# Search link map for memory and key symbols
grep -nE '(_start|app_main|main|text_code_begin|data_code|overlay|ORIGIN|RAM|FLASH)' "$WORK/cpu/br23/tools/sdk.map" | head -200

# Record artifacts and hashes (timestamps matter for catching stale outputs)
find "$WORK/cpu/br23/tools" -maxdepth 2 -type f \
  \( -name 'sdk.elf' -o -name 'sdk.map' -o -name 'app.bin' -o -name 'jl_isd.*' -o -name '*.ufw' \) \
  -printf '%TY-%Tm-%Td %TH:%TM:%TS %s %p\\n' | sort
sha256sum path/to/candidate-artifact
```

## Safe target dry-run

Before running a new SDK target, inspect the Makefile and dry-run it:

```bash
make -n ac635n_spp_and_le
```

Read the full commands and post-build script. A dry-run helps reveal tool paths and side effects, but does not validate the build or package. Do not run any command that connects to a device or performs erase/programming merely to test packaging.
