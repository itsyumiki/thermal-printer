# Architecture

```
Mac ── native USB (CDC 0: log, CDC 1: fm1t) ──► XIAO RP2040 ── GP0/GP1/GND ──► FM-1
```

| | Role |
|---|---|
| core 0 | USB device to the Mac |
| core 1 | USB_KEY recovery (PIO0), then the USB host (Pico-PIO-USB on PIO1) |
| clk_sys | 120 MHz |

## Boot flow

1. If D+ is already pulled up (FM-1 running): 6 s of 1 ms pulses.
   Otherwise: send USB_KEY `0x16EF` until ACK, then 1 ms pulses.
2. Release GP0/GP1 and start the PIO USB host.
3. Stock V15 (`4C4A:C755`): the soft key moves it to UBOOT.
   UBOOT (`4C4A:8057`): ready for `fm1t`.

The ROM trims its clock from the 1 ms pulses. If the pulses stop too early, it boots from flash instead.

## Safety

The firmware writes only whole 4 KiB sectors in `0x4000`–`0x93000` and refuses everything else.

## Source

| File | Contents |
|---|---|
| `src/recovery.c` | USB_KEY and pulses |
| `src/pio_host.c` | USB host and soft key |
| `src/jieli_uboot.c` | UBOOT and loader commands |
| `src/transporter_proto.c` | fm1t protocol |
| `tools/fm1t.py` | Mac client |
