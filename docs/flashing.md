# Flashing, verification, and restoration

## Flashing

Run on the build host (`karna`):

```sh
cd ~/thermal-printer/FM-1-transporter/jl-uboot-tool
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "write 0x3000 /home/yumi/thermal-printer/work/image/printer-blink.slice.bin"
```

1. **Prerequisite:** Printer must be in UBOOT mode (the chip enumerates as `4c4a:2342 BR23UBOOT1.00`).
2. **Safety:** Only sectors `0x3000`–`0x63000` are written. The bootloader and sensitive data (VM/BTIF) are not touched.
3. **Verification:** The tool erases, writes, and then verifies each sector.

## Verification

If the printer boots and runs the new firmware, that is the best verification. To manually verify the flash state:

```sh
# Read back the written range
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "read 0x3000 0x60000 /home/yumi/thermal-printer/work/image/readback.bin"

# Compare the read-back hash
sha256sum /home/yumi/thermal-printer/work/image/readback.bin
```

## Restoration

To revert to the stock firmware:

```sh
# Restore from the saved slice
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "write 0x3000 /home/yumi/thermal-printer/work/image/original.slice.bin"

# OR restore from the full dump
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "write 0 /home/yumi/thermal-printer/work/karna-dump.bin"
```

If the printer does not boot after a write, the boot ROM should automatically drop back into download mode. Power-cycle the printer (or trigger the RP2040 key sequence) to put it back into UBOOT mode, then restore the original dump.
