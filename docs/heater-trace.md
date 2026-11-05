# Heater (PB11) trace log

Firmware option `PB_HEATER_TRACE` (default 1, `firmware/apps/pinbridge/src/app.c`, section "HL"). It answers one question without a scope: how long is PB11 really high, and what were the timer and deadman doing when it toggled.

## How it works

- Every touch of PB11 is recorded in a 40-entry RAM ring: the HAL write and mode paths, the deadman disarm, the TIMER3 ISR cutoff, plus the deadman arm and the executor's `pulse_start` / `pulse_end` (the executor reports its own requested width and what its loop measured).
- The ring is printed as plain text from the command task after each `pb_poll`, never from the code that toggles the pin, so logging does not lengthen a pulse. Entries cost a few microseconds to record.
- Lines go out on the normal USB CDC channel but are not TBP frames; they start with `HL `. The Bun host prints them to stdout as `HL ...` (not as UI warnings). Raw terminals show them as is.
- Lines are dropped, and counted in `lost=`, when the tx ring would drop below 160 free bytes, so TBP replies are never starved. Writes that leave PB11 low are not logged; they are counted in `same=` on the next line.
- Set `-DPB_HEATER_TRACE=0` to compile it all out (the core hook stays, unused).

## Line format

```
HL <seq> t=<us> PB11 <write|mode|disarm|isr_cut> <old>-><new> in=<pin> dir=<0=out> armed=<0|1> dl_left=<us> cnt=<TIMER3 CNT> con=<TIMER3 CON> wraps=<n> [same=N] [lost=N]
HL ...   falls (1->0) add:  hi_us=<high time, timebase> hi_jiffy_ms=<same on the 10 ms jiffy clock> starts= trips= over= lag= stp=
HL ... t=.. arm req_us=<n> deadline=<timebase us>
HL ... t=.. pulse_start req_us=<n> slice=<i>/<n> row_us=<row total> in=<pin>
HL ... t=.. pulse_end req_us=<n> loop_us=<executor's measurement> loop_jiffy_ms=<n> flags=<bit0 over deadline, bit1 abort> in=<pin> ... lag= stp=
```

- `t` is the TIMER3 microsecond timebase. `hi_us` is the real PB11-high time by that clock: compare it with `req_us`. `hi_jiffy_ms` is a coarse cross-check on a different clock (10 ms resolution).
- `in` is the pin read-back through the port IN register right after the write; it can lag the write by a cycle, so trust it on `pulse_end` (read while PB11 is still high) rather than on `write` lines. `in=0` there with `dir=0` would mean the pin is not actually being held high.
- `lag` is the worst TIMER3 ISR lateness in 1/6 us ticks (6000 = a full 1 ms period). Values near 6000 mean interrupts were masked for almost a whole period.
- `dl_left` is the deadman deadline minus now; negative once the deadline has passed.

## What it can and cannot show

It shows the width the firmware's own clock saw, the TIMER3 state, the deadman behaviour and the pin read-back. It cannot show the electrical pulse at the printhead (supply sag, a slow edge, a shorted strobe) and it shares TIMER3 with the thing it measures. A scope on PB11 remains the real check (see the bug doc, section 8.6).
