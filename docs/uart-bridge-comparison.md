# uart-bridge: BLE vs Classic SPP comparison (work in progress)

Status: **variant A (ble-uart-serial) done and linked; variant B (classic-spp-uart-serial) not started.** All numbers come from the scratch harness
(SDK pin fdc018d, real `scripts/apply_app.py`, vendor clang 4.0.1 under qemu), not from karna. [compiled]

## Sizes (ELF sections; `app.bin` = what make-app-bin.sh concatenates)
| build | text | data | bss | app.bin | note |
|---|---|---|---|---|---|
| baseline `hello-cdc`-style (CDC, BLE off, idle app) | 63068 | 18332 | 9000 | 81440 | earlier session, not re-run |
| **A ble-uart-serial** | 133384 | 19096 | 19656 | 152520 | BLE on, EDR off |
| B classic-spp-uart-serial | - | - | - | - | not built yet |

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
