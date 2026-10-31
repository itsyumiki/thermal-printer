# Build system

The build system targets reproducibility and safety: a changed SDK cannot silently build the wrong thing, and the firmware build is decoupled from the hardware-level patching.

## Build host (`karna`)

The Linux build host is responsible for:
1. **Compiling:** JieLi `pi32v2` toolchain (clang) for the `BR23` family.
2. **Post-processing:** creating the binary image (`text.bin`, `data.bin`, etc.) from the ELF and concatenating them into an `app.bin`.
3. **Flashing:** communicating with the chip in UBOOT mode via the RP2040 transporter bridge.

## The pipeline

1. **`scripts/remote-build.sh`:**
   - Syncs `firmware/` and `scripts/` to the build host.
   - Cleans the build host's workspace, clones a pinned SDK commit (`fdc018d`), and runs `scripts/build-app.sh`.
   - Fetches the result (`app.bin`, `sdk.elf`, `sdk.map`).
2. **`scripts/build-app.sh`:**
   - Creates a fresh, isolated build tree.
   - Runs `scripts/apply_app.py` to:
     - Set SDK config macros (e.g., `CONFIG_APP_IDLE 1`).
     - Overlay custom source files (e.g., `src/app.c` over the SDK's example).
   - Runs `make` with `RUN_POST_SCRIPT=true` to skip the vendor's closed-source post-build packaging.
   - Runs `scripts/make-app-bin.sh` to produce the final `app.bin`.
   - Freezes the build time (`faketime`) for reproducibility.
3. **`tools/patch_app.py`:**
   - The final stage, running locally. It patches the new `app.bin` into a copy of `dump.bin`.
   - It performs its own self-test: unpacks the patched image with `fwunpack_newfw.py` and verifies all CRCs, that the app area is correct, and that no bytes were changed outside the allowed `0x3000`–`0x63000` window.

## Reproducibility

- The pipeline uses a pinned SDK commit (`fdc018d`) and verified SHA-256 for the toolchain/helper downloads.
- `faketime` ensures the build-time metadata (embedded by `__DATE__` / `__TIME__`) does not make the binary change every time it is built.
- Two builds of `blink-pc7` on `karna` now result in byte-identical `app.bin` files.
