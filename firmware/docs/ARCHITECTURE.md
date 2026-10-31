# Architecture

```
Mac ── native USB (CDC 0: log, CDC 1: br23t) ──► XIAO RP2040 ── GP0/GP1/GND ──► printer
```

| | Role |
|---|---|
| core 0 | USB device to the Mac |
| core 1 | USB_KEY recovery (PIO0), then the USB host (Pico-PIO-USB on PIO1) |
| clk_sys | 120 MHz |

## Boot flow

1. If D+ is already pulled up (printer running): 6 s of 1 ms pulses.
   Otherwise: send USB_KEY `0x16EF` until ACK, then 1 ms pulses. Switch the printer ON while the key runs.
2. Release GP0/GP1 and start the PIO USB host.
3. UBOOT (`4C4A:2342`): ready for `br23t`. (The FM-1's stock-V15 soft key is still in the code but cannot trigger on the printer.)

The ROM trims its clock from the 1 ms pulses. If the pulses stop too early, it boots from flash instead.

## Safety

The firmware writes only whole 4 KiB sectors in `0x3000`–`0x63000`, and only when the chip reports key `3496` / flash id `856013`. It refuses everything else.

## Source

| File | Contents |
|---|---|
| `src/recovery.c` | USB_KEY and pulses |
| `src/pio_host.c` | USB host and soft key |
| `src/jieli_uboot.c` | UBOOT and loader commands |
| `src/transporter_proto.c` | br23t protocol |
| `../host/br23t.py` | host client |
