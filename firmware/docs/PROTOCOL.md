# JieLi BR23 UBOOT protocol (printer profile)

Inherited from the FM-1 Transporter's WL82 notes. The printer-specific parts (loader, flash id, layout) come from the printer's dump and a working `info`/`dump` run; the rest is unchanged protocol and was not re-verified here.

## Entry

- USB_KEY `0x16EF`, MSB first, D+ = clock and D- = data. Only this polarity works. The Pico sends it while the printer powers on.
- After the ACK the ROM pulls D+ high. Send 1 ms pulses until a host takes over.
- The ROM accepts the key at power-on and after a watchdog reset.
- (FM-1 only: stock V15 also enters UBOOT on a USB-MIDI packet. The printer has no such path.)

## Device

UBOOT shows up as `4C4A:2342` "BR23UBOOT1.00" (the firmware also still accepts the FM-1's `4C4A:8057`): USB Mass Storage, Bulk-Only Transport.

Each command is a 16-byte CDB: `cmd` (u16, big-endian) followed by its arguments, padded with `0xFF`.

| cmd | Name | Stage |
|---|---|---|
| `FB06` | WRITE_MEMORY | ROM |
| `FD07` | READ_MEMORY | ROM |
| `FB08` | JUMP | ROM |
| `FC09` | READ_KEY (expect `3496`) | loader |
| `FC0A` | GET_ONLINE_DEVICE (expect `856013`) | loader |
| `FD05` | READ_FLASH | loader |
| `FB01` | ERASE_SECTOR (4 KiB) | loader |
| `FB04` | WRITE_FLASH | loader |
| `FC0C` | RUN_APP | loader |

- Upload `br23loader.bin` (11264 bytes, jl-uboot-tool) to `0x12000`, then send `JUMP(0x12000, 1)`.
- In the ROM stage, memory data is XORed with `jl_crc_cipher`. Flash data through the loader is not ciphered. The loader blob itself is shipped already ciphered.
- The ROM runs a watchdog. The loader resets the chip after about 3 s without a command; the Pico sends a keepalive after 1 s of silence.

## Flash layout (512 KiB)

Measured from the printer's dump.

| Range | Contents | Pico writes |
|---|---|---|
| `0x00000`–`0x01000` | blank | never |
| `0x01000`–`0x03000` | top directory, `uboot.boot` (ends `0x3088`) | never |
| `0x03000`–`0x03100` | end of `uboot.boot`, `isd_config.ini` | only as part of sector `0x3000`, unchanged |
| `0x03100`–`0x63578` | `app_dir_head`, entry list, `app.bin` (from `0x3220`), `cfg_tool.bin`, tone | allowed: `[0x3000, 0x63000)` |
| `0x64000`–`0x7E000` | VM storage | never |
| `0x7E000`–`0x80000` | BTIF, `key_mac` | never |

The app area (`0x3100` up) is scrambled with the chip key (`3496`). `host/patch_app.py` documents the entry format.

## Transporter data channel

Text lines over the second CDC port; see the header of `firmware/src/transporter_proto.h`. Writes (`wsec`) are refused with `ERR range` outside the window and `ERR identity` unless the chip reports key `3496` / flash id `856013`.
