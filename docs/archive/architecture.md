# Architecture and terminology

## Device facts and open questions

The target board is marked `JL BP21568-56C4`; the working identification is JieLi AC6956C / BR23-family. The original dump is 512 KiB and has been associated with flash ID `0x856013`, chip key `0x3496`, and entry address `0x01E00120`. These facts are useful anchors, but they do **not** prove which exact vendor boot ROM, mask ROM ABI, chip revision, flash geometry, RF configuration, or bootloader update protocol a replacement image must use.

The Soundbox SDK contains a board directory called `board_ac695x_smartbox`, and the official Bluetooth SDK has a BR23 board family targeting AC635N. Shared BR23/pi32v2 naming suggests a related toolchain/ISA ecosystem; it is not enough to establish that an AC635N image can run on an AC6956C. Chip support must be confirmed against the board, original image, ROM APIs and SDK support matrix.

## Important distinctions

- **Source code**: C/C++/assembly and headers in an SDK.
- **Prebuilt libraries**: vendor-supplied `.a` archives or LLVM bitcode/object libraries. Their source may not be present. They can contain chip-specific assumptions.
- **Compiler output**: `.o` files; in this toolchain, `-flto` can make these contain LLVM bitcode rather than final machine code.
- **Linker ELF**: `sdk.elf`; contains linked code/data, section/symbol/debug metadata, and addresses from the linker script. It is not automatically the file that a ROM/ISP tool expects to flash.
- **Link map**: `sdk.map`; explains where sections and symbols were placed and helps diagnose size/overlap problems.
- **Section binaries**: `text.bin`, `data.bin`, overlay payloads, etc., extracted or generated from the ELF for packaging.
- **Application payload**: concatenated code/data/overlay sections, often `app.bin`. It may omit bootloader, headers, configuration, checksums, encryption, or updater metadata.
- **Download package**: SDK-generated `jl_isd.bin`, `.fw`, `.ufw`, or other package, depending on SDK/tool version and update mode. These are not interchangeable.
- **Whole-flash dump**: an address-preserving representation of flash contents. It is different from an application payload or update package.

## Why 512 KiB is a hard constraint

512 KiB is 524,288 bytes total, including any bootloader, reserved config, NV/VM data, update metadata, and application regions that reside in the same flash. An application binary of 792,762 bytes cannot fit in 512 KiB even before reserving any of those regions. A `CONFIG_FLASH_SIZE` macro may affect generated configuration and package layout but does not, by itself, shrink the linked image: linker MEMORY regions, selected features, libraries, and post-build layout all matter.

## Bluetooth, HID, and USB

The official Bluetooth SDK includes distinct HID and SPP/BLE example applications. Bluetooth HID means a Bluetooth HID profile; it does not imply USB HID. A thermal printer may use Bluetooth Classic SPP, BLE GATT, or a proprietary protocol; determine the original transport/protocol before choosing an example. Physical USB support also depends on the board's wiring, USB-capable controller/PHY and SDK drivers, not just an application target name.
