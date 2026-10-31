# Quickstart

Build and flash the blink firmware from a clean checkout.

## Requirements

1. **Local machine** (Mac or Linux): git, Python 3, `ssh karna` configured without password.
2. **Build / flash host (`karna`)**: Linux x86_64 with `sudo`, connected to the printer over USB via the FM-1 Transporter board.
3. **The target printer**: board `JL BP21568-56C4`, AC6956C chip.
4. **`dump.bin`**: original 512 KiB flash read at the repo root. Hash: `b46ea9a96d6b603b472c7d4551f958e708be3b36375a50314b8f9aeea89de32c`.

## Step 1: One-time build-host setup

Run on your local machine:

```sh
ssh karna sh -s < scripts/setup-build-host.sh
```

This installs Debian build packages, downloads the JieLi toolchain (`/opt/jieli`) and post-build helper (`/opt/utils`), clones the SDK commit (`~/jl/fw-AC63_BT_SDK`), and creates `~/jl/venv`. Idempotent: safe to run again.

## Step 2: Build the firmware app

Run on your local machine:

```sh
sh scripts/remote-build.sh blink-pc7
```

This syncs `firmware/` and `scripts/` to `karna`, builds `blink-pc7` in a fresh SDK clone at `~/jl/build/blink-pc7`, and fetches the result into `build/blink-pc7/`.

Artifacts generated:
- `build/blink-pc7/app.bin` (76,016 bytes): application binary
- `build/blink-pc7/sdk.elf`, `sdk.map`, `build.log`, `BUILD-INFO.txt`

## Step 3: Patch `app.bin` into the flash image

Run on your local machine:

```sh
python3 tools/patch_app.py \
  --dump dump.bin \
  --app build/blink-pc7/app.bin \
  --out work/image/printer-blink.bin \
  --slice-out work/image/printer-blink.slice.bin
```

Output:
- `work/image/printer-blink.slice.bin` (393,216 bytes = 0x60000): changed sector range `0x3000`–`0x63000`.
- Self-check verifies: CRCs, round trip, `cfg_tool`/`tone`/`VM`/`PRCT`/`BTIF` uncorrupted, and write bounds stay inside `[0x3000, 0x7E000)`.

## Step 4: Copy the slice to karna

```sh
ssh karna "mkdir -p ~/thermal-printer/work/image"
cat work/image/printer-blink.slice.bin | ssh karna "cat > ~/thermal-printer/work/image/printer-blink.slice.bin"
```

## Step 5: Put chip into UBOOT mode

1. Ensure the FM-1 Transporter RP2040 is wired to the printer's D+/D- lines and plugged into `karna`.
2. Power-cycle the printer (or trigger the USB_KEY sequence via the transporter).
3. On `karna`, verify the chip enumerated:

```sh
ssh karna "lsusb | grep 4c4a; ls /dev/sg*"
```

Expect output like:
```text
Bus 001 Device 007: ID 4c4a:2342 BR23UBOOT1.00 BR23UBOOT1.00
/dev/sg0
/dev/sg1
```

Confirm `/dev/sg1` is the printer (not `/dev/sg0`, which is the host's drive).

## Step 6: Flash

Run on `karna`:

```sh
cd ~/thermal-printer/FM-1-transporter/jl-uboot-tool
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "write 0x3000 /home/yumi/thermal-printer/work/image/printer-blink.slice.bin"
```

Expected log:
```text
Waiting for [/dev/sg1] try! ok (BR23 UBOOT1.00 1.00)
Chip: BR23 - AC635N/AC695N series
Running loader with argument 0x0001.
The Loader has been successfully installed.
================ Quick info ==================
  ** BR23 (AC635N/AC695N series) **
  >> Chip key: 0x3496 <<
  - Online device:
     ID: 0x856013
     Type: 0x03 (SPI NOR flash on SPI0)
==============================================
Erasing: 100%|█████████████████████████████████████████| 384k/384k [00:00<00:00, 1.14MB/s]
Writing: 100%|██████████████████████████████████████████| 384k/384k [00:02<00:00, 145kB/s]
```

The LED on PC7 should start blinking every 500 ms immediately after boot.

## Step 7 (Optional): Verify read-back

Re-enter UBOOT mode, then run:

```sh
cd ~/thermal-printer/FM-1-transporter/jl-uboot-tool
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "read 0x3000 0x60000 /home/yumi/thermal-printer/work/image/readback.bin"
sha256sum /home/yumi/thermal-printer/work/image/readback.bin
```

Expected hash: `5bfcdff28de20df9db13fbdc8dceb6bd08b4fc459141cc126ccee4de62d3119c`.

## How to restore original firmware

If the printer needs to be reverted to stock:

1. Re-enter UBOOT mode.
2. Run on `karna`:

```sh
cd ~/thermal-printer/FM-1-transporter/jl-uboot-tool
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "write 0x3000 /home/yumi/thermal-printer/work/image/original.slice.bin"
```

Or restore from the full dump (`/home/yumi/thermal-printer/work/karna-dump.bin` or `dump.bin`):

```sh
sudo ~/jlvenv/bin/python jluboottool.py --device /dev/sg1 \
  "write 0 /home/yumi/thermal-printer/work/karna-dump.bin"
```
