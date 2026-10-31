# Hardware and flash layout

## The target board

- **Marking:** `JL BP21568-56C4`
- **Chip:** JieLi AC6956C (BR23 architecture, `pi32v2` 32-bit RISC core)
- **Flash:** 512 KiB SPI NOR flash (`0x80000` bytes)
- **Flash ID:** `0x856013` (density code `0x13` = $2^{19}$ = 512 KiB)
- **Chip key:** `0x3496` (factory scrambling seed)
- **Entry point:** `0x01E00120`

## Flash map (`dump.bin`, 524,288 bytes)

Analyzed via `fwunpack_newfw.py` and direct inspection.

| Range | Size | Contents | Writes permitted? |
|---|---|---|---|
| `0x0`–`0x1000` | 4 KiB | Unused / blank in factory dump | Never |
| `0x1000`–`0x3000` | 8 KiB | Second-stage bootloader (`uboot.boot`) + `isd_config.ini` | Never (`patch_app.py` preserves it) |
| `0x3000`–`0x3100` | 256 B | Sector holding `isd_config.ini` tail and `app_area_head` entry | Modified by `patch_app.py` |
| `0x3100`–`0x63578` | ~393 KiB | Application area (`app.bin`, `cfg_tool.bin`, resource files) | **Written** (`patch_app.py` replaces `app.bin` and re-encrypts) |
| `0x63578`–`0x64000` | ~2.6 KiB | Padding / slack in sector | Preserved |
| `0x64000`–`0x7e000` | 108 KiB | VM (Variable Memory / non-volatile storage area) | Preserved |
| `0x7e000`–`0x7f000` | 4 KiB | BTIF (Bluetooth Info / MAC address / pairing keys) | Preserved |
| `0x7f000`–`0x80000` | 4 KiB | Reserved / `key_mac` | Preserved |

### Why `patch_app.py` writes `0x3000`–`0x63000`

The original app area in the dump runs from `0x3100` to `0x63578`. `patch_app.py` operates on 4 KiB aligned sectors. The 384 KiB slice from `0x3000` to `0x63000` (96 sectors) covers every byte that changes between the stock image and the blink image, while leaving the bootloader (`< 0x3000`) and the VM/BTIF (`>= 0x64000`) completely untouched.
