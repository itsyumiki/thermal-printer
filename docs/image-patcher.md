# Image patcher: `tools/patch_app.py`

This tool takes the original `dump.bin` and patches in a new application payload. It is safer than the vendor packager because it does not change the printer's own layout, boot config, or key data.

## How it works

1. **Baseline:** uses your original 512 KiB dump as the only source of truth for the header, loader, configuration and protected NV storage.
2. **Decryption:** uses the known chip key `0x3496` to decrypt the application area (`0x3100`–`0x63578`) in RAM.
3. **Patching:** replaces the `app.bin` entry in the entry list, and overwrites the application data.
4. **Correction:** updates the `app.bin` entry's size and data CRC, then the `app_area_head` entry's CRC.
5. **Encryption:** re-encrypts the application area using the same key `3496`.
6. **Validation:** unpacks its own result with `fwunpack_newfw.py` to confirm that all CRCs are valid, that the app round-trips, and that `cfg_tool.bin` and the tone area are bit-identical to the original.
7. **Boundaries:** it enforces that no bytes are written outside `0x3000`–`0x7E000`.

## Trustworthiness

The patcher is as trustworthy as the `fwunpack_newfw.py` script provided by the community for the JieLi file format. It is a logic-level patcher: it changes only the bytes it has to change and validates its work with the same rules the boot ROM uses to check the firmware's validity.
