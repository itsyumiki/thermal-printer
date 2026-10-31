# Archive: early research notes

These are the notes written **before** anything was flashed, while the Soundbox SDK and the official Bluetooth SDK were still being compared. They are kept as a research log. **Do not follow them as instructions.** Known problems:

- They refer to `sdk/`, `toolchain/` and `artifacts/ac6956c-512k-build/` directories that do not exist in this workspace. The SDKs sit at the repo root, and the toolchain lives on the Linux build host in `/opt/jieli`.
- They say "no firmware has been validated or flashed". That is no longer true: a custom firmware was flashed and runs (see [`../journal.md`](../journal.md)).
- They suggest packaging with the SDK's own post-build step (`isd_download` / `host-client`). That path does not work here (see [`../troubleshooting.md`](../troubleshooting.md)). The working procedure is in [`../quickstart.md`](../quickstart.md).
- They treat the Soundbox SDK (AC695N) as a candidate. Its stock payload (~790 KB) is larger than the printer's whole 512 KiB flash, so it is not usable as-is.

Current documentation: [`../README.md`](../README.md).
