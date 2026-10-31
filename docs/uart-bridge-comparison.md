# uart-bridge: BLE vs Classic SPP comparison

Status: **variant A (ble-uart-serial) built, flashed and tested on hardware (2026-11-01): both directions work.** Variant B (Classic SPP) is
**not being built** (owner decision, see "Decision" below). Size numbers come from the scratch harness (SDK pin fdc018d, real
`scripts/apply_app.py`, vendor clang 4.0.1 under qemu), not from karna. [compiled]

## Sizes (ELF sections; `app.bin` = what make-app-bin.sh concatenates)
| build | text | data | bss | app.bin | note |
|---|---|---|---|---|---|
| baseline `hello-cdc`-style (CDC, BLE off, idle app) | 63068 | 18332 | 9000 | 81440 | earlier session, not re-run |
| **A ble-uart-serial** | 133384 | 19096 | 19656 | 152520 | BLE on, EDR off |
| B classic-spp-uart-serial | - | - | - | - | not built (decision below) |

Flash limit: the harness linker script has `code0 LENGTH 0x80000` (512 KiB) at `0x1E00120`; the printer's app area is 0x3000..0x63000 (393216 B,
thermal-printer README). A's `app.bin` (152520 B) fits both. No overlay banks in the ELF (`.overlay_bank0` empty, so make-app-bin.sh's check passes). [compiled]

## Do the overrides take effect under LTO? (A)
Yes, by the linker's own resolution file (`sdk.elf.resolution.txt`): the core's definitions of `log_print printf puts putchar put_buf` and of
`ble_/l2cap_/profile_/rfcomm_debug_enable` are the only *prevailing* ones (flag `pl`, input `ble_24g_deal.c.o`); the library definitions in
`system.a` (log.c, printf-stdarg.c) are not prevailing. `vsnprintf` still resolves to the library (`printf-stdarg.c`). The link has
`-dont-used-symbol-list=...printf,puts,putchar` which only stops LTO from deleting those names as unused, so the override is kept.
The final ELF has one `log_print`/`printf`/`puts`/`putchar`/`put_buf` each (local `t` symbols, LTO-internalised). The core's strings ("[BT] ") and "JL-UART-BLE" are in the ELF.
Not proven: runtime behaviour. [compiled] [untested on hardware]

## Warnings
The harness compiles with `-w`. The core and the glue TU were re-compiled with `-Wall -Wextra`: the core has no warnings; `ble_trans.c` only has the
SDK's original ones. The core also compiles with `BRIDGE_CDC_LOGS=0`, `BRIDGE_RX_VIA_SDK_READ=1`, `BRIDGE_BT_STACK_DEBUG=0`. [compiled]

## Hardware test, variant A (2026-11-01) [tested on hardware]
Setup: owner built on karna, flashed, board on USB, macOS host. Serial side `cat`/`tio` on the CDC port, BLE side
`firmware/apps/ble-uart-serial/tools/connect.py` (bleak/CoreBluetooth). Only what the captured logs show is claimed here.

Worked:
- Enumerates as USB CDC, the host's line-coding changes (9600 -> 115200) show up in the log. SDK UART0/PA0 log is off, all logs arrive on CDC with the `[BT] ` prefix.
- Advertises and answers as `JL-UART-BLE` (log: `read gap_name: JL-UART-BLE`). GATT as the SDK trans_data default: service `ae30` with
  `ae01` (write, write-without-response), `ae02` (notify), `ae03` (write-without-response), `ae04` (notify), `ae05` (indicate), `ae10` (read, write),
  plus a second SDK default service `ae3a` (`ae3b` write-without-response, `ae3c` notify).
- ATT MTU 185 negotiated; the app switched its chunk size 20 -> 182 (`app: ATT MTU 185 -> notify payload 182`). CCC write on `ae02` was seen
  (`app: ae02 notifications enabled`).
- **BLE -> CDC:** 15 bytes written to `ae01` (write-without-response) came out of the serial port as `hello from mac`; stats `bt_rx=15 b2c=15`.
- **CDC -> BLE:** 5 characters typed into the serial terminal arrived as 5 `ae02` notifications (one byte each: human typing is slower than the 10 ms idle flush).
- Ring counters at that point: `drops nopeer=0 ringA=0 ringB=0 fail=0 cdc=0 busy_events=0`.

Not tested: re-advertising after a disconnect, bursts/sustained throughput, write-with-response path, ring overflow, `BRIDGE_CDC_LOGS=0`,
any central other than macOS, long runs.

## Problems found (TODO, nothing fixed yet)
1. **`app_key_evnet: 2,0` log spam** several times per second, which eats most of the log bandwidth and drives most of the `bridge: log lines dropped` counter.
   Cause (from the board config, not yet confirmed on hardware): the SDK demo board config has `TCFG_ADKEY_ENABLE ENABLE_THIS_MOUDLE`
   on `IO_PORTB_01` (ADC channel `AD_CH_PB1`, external pull-up assumed). An ADKEY is a button read through the ADC: several buttons share one pin via a
   resistor ladder and the firmware decodes the voltage into a key number. If PB1 is not wired like the demo board, the pin floats/reads noise
   and the SDK decodes key events from it. `TCFG_IOKEY_ENABLE` (plain GPIO keys) is already off. Proposed fix: a `set` line in `app.conf`
   (`TCFG_ADKEY_ENABLE DISABLE_THIS_MOUDLE`), which also stops the ADC sampling PB1. The printer's real pin map is still an open task (docs/journal.md), so confirm PB1 is not used by the board before shipping. [untested]
2. **Unexplained USB drop/reboot.** In the capture the serial device disappeared about 2 s after `tio` attached (right after the 5 notifications) and the board came back with a full boot log. No log line precedes it.
   If the board was not unplugged or reset by hand, this looks like a firmware reset (crash/watchdog) under the typed-input path, which needs to be reproduced and chased first. If it was replugged, ignore.
   Reset-source line in both boots reads `VDDIO POR`, so it does not tell which. [open]
3. **Boot logs mostly lost.** About 100+ lines are dropped at boot (`bridge: log lines dropped, total N`) because the host has not opened the CDC port yet and the 3 KiB log ring fills. Options: bigger ring, or hold log lines until the host opens the port. [untested]
4. **Slow connection parameters.** The peripheral requests interval 16-24 (1.25 ms units), **slave latency 10**, timeout 600 (SDK example defaults) and the central accepts: interval 30 ms, latency 10.
   With latency 10 the board may skip up to 10 connection events, so central -> board writes could be delayed by up to roughly 300 ms [estimate, not measured]. Fix: request latency 0 in the example's connection-parameter table. [untested]
5. Cosmetic: the SDK prints stray `#` / `X` characters (`[BT] #`) now and then. Not investigated.

Correction to an earlier read of the capture: text such as `h[BT] ...`/`el[BT] ...` seen in the `cat` window is the terminal echoing the keys typed into that window, not the bridge
mixing data and logs (`cat` on the device does not read the keyboard; the same keystrokes reached the board through the `tio` session). Nothing indicates a data/log separation problem yet.
Also, the `end_addr:63600` boot-log line is the end of the image file table, not a free-space figure; use the `size`/`app.bin` numbers above for flash fit (152520 B of 393216 B app area in the harness).
Those two do not obviously reconcile and have not been cross-checked on karna's real build.

## Decision: Classic SPP variant (B) not built
Owner decision 2026-11-01: stay BLE-only. Reasons (estimates are labeled):
- Cross-platform reach: BLE works from iOS, Android, macOS, Windows, Linux and Web Bluetooth. Classic SPP cannot be used from iOS apps without Apple's MFi program and has no Web Bluetooth.
- The printer protocol this project targets already runs over BLE (service `ae30`), so Classic would be a second transport for the same data.
- Throughput: Classic SPP is typically faster than BLE [estimate], but a print line is small (384 px / 8 = 48 bytes), so BLE is expected to keep the head fed. Connection parameters (item 4) matter more than the radio mode. [estimate]
- Cost: Classic pulls in the BR/EDR half of the controller and the SPP/RFCOMM host code (`btctrler.a`/`btstack.a` contain both; `TCFG_USER_EDR_ENABLE` is the switch), i.e. more flash/RAM and a second stack to debug.
- When to revisit: an SPP-only peer (old Android app, PC tool opening a Bluetooth COM port), or bulk transfers (e.g. firmware update over BT). A compile-only Classic build for size would still be cheap if the number is ever wanted.
