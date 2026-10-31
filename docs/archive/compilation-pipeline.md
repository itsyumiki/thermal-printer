# What source code goes through when compiling

This describes the normal pipeline in these JieLi SDKs. Exact details vary with the compiler version, source language, build flags, archive format, and post-build scripts. The flow is not the same as a generic Linux executable build: the output is firmware for a vendor-specific embedded target.

## 1. Configuration and preprocessing

Make selects a board and defines feature macros (for example `CONFIG_CPU_BR23`, soundbox/audio or HID/SPP profile flags, flash size, update mode). The compiler preprocesses each translation unit: it expands `#include` files and macros, removes inactive `#if` branches, and processes conditional code. The resulting translation unit may differ substantially between board configurations. Incorrect feature defines can select incompatible drivers or leave out required initialization.

Useful diagnostics: build command lines, `-E` preprocessor output where supported, compiler include tracing, and checking which board header was actually included. Never infer the active board from a header file's existence alone.

## 2. Compilation and target-specific code generation

Clang parses C/C++ (and assembly through the integrated assembler), checks types and language rules, and creates LLVM IR or object output. Target flags such as `-target pi32v2 -mcpu=r3` choose JieLi's embedded architecture and instruction/ABI model; `-Oz` requests size-oriented optimization; `-g` emits debug metadata. `-flto` allows the linker plugin to optimize across translation units later, so intermediate `.o` files may contain LLVM bitcode rather than fully finalized machine instructions.

Compilation can succeed while later link fails because unresolved symbols, unsupported target features, incompatible bitcode, or missing vendor libraries appear only at link time.

## 3. Assembly and object files

Assembly is translated into target object sections, symbols and relocations. An object typically has code/data sections, symbol tables and relocation records. In LTO builds some object contents remain IR. The per-source outputs are usually in a build directory such as `objs/`; the Makefile's generated object list determines which are linked.

## 4. Static libraries and prebuilt vendor modules

`.a` files are archives of object files/bitcode. The linker extracts members needed to resolve symbols, subject to archive order, link flags, and section garbage collection. Vendor libraries may be prebuilt for a different target triple or toolchain version. The recorded Soundbox build warned that a `lib_cpu.a` module targeted `pi32` while the current link targeted `pi32v2`; this warning merits investigation even though the link returned success. It is unsafe to assume similarly named targets are binary-compatible.

## 5. Link-time optimization and final link

The link command invokes JieLi's `lto-wrapper`/LLVM gold plugin and `lto-ar` ecosystem, resolves references across objects and libraries, performs LTO optimization, places sections according to the linker script, assigns addresses, resolves relocations and creates `sdk.elf`. The linker script defines code flash origin/length, RAM and optional PSRAM, section ordering, reserved areas and ROM-function absolute addresses. These are critical to boot compatibility.

The map file (`sdk.map`) records the memory placement and symbol/section accounting. Review:

- entry symbol/address and startup symbols;
- flash code origin and declared length;
- code, data, overlay and bank regions;
- RAM/PSRAM placement and overflow;
- reserved loader/update/NV areas;
- undefined symbols, discarded sections and warnings;
- total payload bytes after extraction, not just ELF file size.

The ELF's file size is not the amount of flash consumed. Debug sections and symbol tables can make an ELF much larger than its loadable firmware sections. Conversely, a small ELF can describe code linked at the wrong address.

## 6. Section extraction and post-build transformation

A post-build script may extract selected ELF sections with `objcopy`, concatenate sections, trim padding, generate config files, append version/chip data, compress payloads, compute checksums, or invoke packagers. Some SDKs have distinct outputs for factory/ISP programming, serial updater, Bluetooth OTA, and JL-OTA. The same filename in another build tree can be stale; always record timestamps, hashes, command logs, and the output directory.

A manually extracted raw application payload can help measure code size, but it is not equivalent to a vendor package. If any helper executable is unavailable, do not silently skip its transformation or copy old package files into the new output directory.

## 7. Firmware package generation

The package tool may wrap payloads with headers and metadata (chip/PID/VID, flash size, entry or offsets, version, update mode, alignment, checksums, encryption/signatures, compressed lengths). The exact contract is SDK/tool-version-specific. Confirm it from the SDK's scripts, source, tool help, and known-good package structure. `app.bin`, `jl_isd.bin`, `jl_isd.fw`, `update.ufw`, and a whole-chip dump should be treated as different formats unless proven otherwise.

## 8. Validation and programming

Before a device is programmed, independently verify package structure, expected chip and flash ID, addresses/lengths against the actual 512 KiB geometry, checksums, and reserved regions. Preserve the original complete dump and recovery route. Only then consider the correct vendor programming/update tool. Build success, a plausible filename, or a small enough file is not sufficient proof of flashability.
