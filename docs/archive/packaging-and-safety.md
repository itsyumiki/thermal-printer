# Packaging, flashing, and safety checklist

## Package types are not interchangeable

| Artifact | What it normally represents | Flashable by itself? |
|---|---|---|
| `sdk.elf` | Linked program plus symbols/debug metadata and section addresses | No, not by ordinary ISP tools |
| `sdk.map` | Linker accounting and address map | No |
| `text.bin` / `data.bin` / overlay binaries | Individual extracted sections | No, unless a documented tool specifically accepts them |
| `app.bin` | SDK-specific concatenated application payload | Do not assume so |
| `jl_isd.bin` / `jl_isd.fw` | JieLi toolchain/version-specific download/firmware containers | Only with the matching documented tool and target config |
| `update.ufw` | Often an update/OTA package | Only through a matching update mechanism |
| Whole-flash dump | Bytes corresponding to flash addresses, often including loader/config/NV | Not interchangeable with application/update packages |

Use the exact definitions in the checked-out SDK and the tool version that creates the artifact. A filename alone is not a format specification.

## 512 KiB arithmetic

- Total nominal capacity: `512 * 1024 = 524288` bytes.
- Fresh Soundbox payload measured from the 512 KiB-configured ELF: `792762` bytes, which is `268474` bytes larger than total flash.
- Therefore this payload cannot fit into the device's 512 KiB flash, even before bootloader, configuration, updater, NV storage or alignment overhead. It is a diagnostic artifact only.
- The stock 1 MiB experiment's application payload is documented separately; it is not a whole-flash image and is not suitable for this device based on the current evidence.

## Why post-build matters

The observed Soundbox Make run returned exit code 0 and linked a new ELF/map, but package outputs retained older timestamps. Thus the package stage was not confirmed to have regenerated those files. Stale package outputs must never be presented as corresponding to the newest source/configuration. The raw extracted `app.bin` in `artifacts/ac6956c-512k-build/` is clearly named as a payload and is not a final flash package.

## Before any flash operation

- [ ] Confirm exact chip and revision from reliable silicon/SDK documentation, not just a family name.
- [ ] Confirm flash geometry and vendor flash ID; 512 KiB capacity is a hard limit.
- [ ] Preserve at least two verified copies of the original full dump and its hash.
- [ ] Identify bootloader/loader, code origin, interrupt vectors, ROM stubs, update metadata and NV/config offsets from the original firmware and board evidence.
- [ ] Confirm the exact package type and its matching packager/downloader version.
- [ ] Check chip/PID/VID, flash size, entry point, offsets, lengths, checksums, compression/encryption and alignment in the generated package.
- [ ] Verify that the entire image and all reserved regions fit the actual flash.
- [ ] Inspect all build warnings, especially target triple/ABI mismatch.
- [ ] Have a tested recovery path and appropriate physical access before programming.
- [ ] Explicitly confirm intent before any tool operation that can erase/program the chip.

No device programming was performed as part of the experiments documented here.
