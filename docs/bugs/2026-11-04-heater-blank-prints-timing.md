# Heater: blank prints at stock-derived pulse widths, dark-ish at 50 ms+ (timing calculation suspect)

Status: **open**. Written 2026-11-04 from the owner's bench observations plus a source/doc review. Nothing here was
measured with a scope; every number is tagged where it comes from.

## 1. Symptom

- The current firmware (last three commits before `dda457a`: `2bfc4de` heater timer fix, `fb0f5c6` and `368369e` UI only,
  so effectively the `2bfc4de` firmware) prints **completely blank paper**. No mark at all, only a head temperature that
  rises from about 20 C to about 30 C after a few print attempts. [owner]
- Reflashing the **stock firmware** shows the heater hardware itself is fine. The fault is in how this firmware handles
  the heater. [owner]
- With the limits raised in `dda457a` (test only), the print becomes **dark-ish at 50 ms and above**. [owner]
  Assumption to confirm: "50 ms" is the WebUI "Heat per row, total" value (four slices, so about 12.5 ms per event).
  Results between 10 ms and 50 ms were not recorded.
- Before the limit change the maximum was 2.5 ms per event and 10 ms per row, and prints were blank. [owner, code]

Conclusion so far: the head and paper respond, so this is an **energy / pulse-width problem**, not polarity, data, latch or a
dead heater. The open question is why the stock-derived widths are 20 to 70 times too small.

## 2. What we assumed (the numbers that now look wrong)

| Assumption | Source | Status |
|---|---|---|
| PB11 high = heater on, PB11 idles low | Stock TIMER2 ISR sets PB11 only inside the heat window; stock clears it at init and before every shift; owner's 2026-11-02 dotted-line print drove PB11 high and printed | Consistent, not the problem |
| Stock heat tick is about 30 us | `re/docs/printhead-protocol-and-heating.md` section 5.1: TIMER2 `PRD = (144000 / divider) / 200`, 24 MHz oscillator, so 6 ms / 200 ticks | **[derived, never measured]** |
| Stock per-event heat is 25..75 ticks = 0.72..2.22 ms, four events per row, hard cutoff 200 ticks (6 ms) | Same document, sections 5.3, 7, 8 | **[derived]** |
| Therefore a per-event cap of 2500 us and a row cap of 10000 us were "near stock" | `pb_tuning.h` (owner decision 2026-11-02) | **Contradicted by the 50 ms+ result** |
| TIMER3, oscillator, divide by 4, gives 6 ticks per us, so `ticks = usec * 6` | `app.c`, SDK `mcpwm.c` timer setup | **[unverified on hardware]** (see 4.2) |

## 3. Reference firmware comparison

`scratch-bridge/thermal-printer-checker-reference` (commit `c98820d`) is the last old firmware the owner considers to have
satisfactory row timing and darkness.

- Its `hal_delay_us` is a NOP loop, `us * 12` iterations. The 2026-11-02 profile in `docs/journal.md` measured it as
  about **10.9x too long** (latch 0 / 1000 us took 1.9 / 12.8 ms). The demo default `--heat-us 2000` would then have
  been roughly 22 ms per event.
- **The owner says that firmware had huge timing problems and does not think the real pulse was 20+ ms.** So the
  reference's effective pulse width is **unknown**, and it cannot be used as a calibration. The 10.9x figure came from
  the latch delay, not from a measurement of the heat pulse itself.
- The reference only proves that its combination of shift, latch, heat pulse and stepping produced good darkness.

## 4. Hypotheses for the timing error (none confirmed)

1. **The 30 us stock tick is wrong.** The derivation assumes the `144000` constant means 6 ms at 24 MHz and that the
   timer runs from the 24 MHz oscillator. If the real tick is larger, the stock widths are longer than 0.72..2.22 ms and
   our "stock-equivalent" cap was far too low. Check: scope PB11 during a stock print.
2. **Our TIMER3 does not count at the rate we think.** The only hardware evidence is the pre-fix bench `PHDIAG`
   (`cnt=21177`, PRD 3750, build `E51DB80`, with the capture-mode bit set). It is roughly consistent with 6 ticks per us
   once the polling loop's overhead is included, but that was in the wrong mode. Check: `PHDIAG` after a heated row on the
   current build, and a scope on PB11 for a known requested width (for example requested 20 ms vs measured).
3. **Software timing helpers are off.** `hal_delay_us` is still a calibrated NOP loop (about 1.1 iterations per us,
   calibrated once, 2026-11-02). The shift, latch and guard delays and the in-line pulse bound depend on it. The journal
   also lists the 3 ms shift time as an estimate.
4. **Step tick may not be 1 ms / 2 ms as assumed** (journal: "suspected 2 ms step tick"; the WebUI "Measure step tick"
   button was added for this). Heat slices are gated on completed motor edges, so row timing and energy per row depend on
   it.
5. **Energy per dot is not what stock delivers** even at the same width: head supply or battery sag, cold head, a
   different stock event rhythm (stock reshifts and relatches the row every event, we latch once and fire four slices),
   or stock quality / energy bytes that give a larger T than the typical 25..75 ticks. Not investigated.

## 5. State of the repo

- `dda457a` (pushed to `origin/pinbridge`): **test-only** heat limits, per-event 30000 us, per-row 60000 us (the row
  field is a u16, 65535 max), heater timer prescaler /16 (1.5 ticks per us), duty budget unchanged at 60000 us per 200 ms.
  Host and WebUI no longer enforce any heat limit; the firmware does. Temperature (70 / 75 / 50 C), battery and paper
  gates are unchanged.
- These values are experimental and must be restored or re-chosen once the real pulse width is understood.
- `host/pinbridge/bun/server.ts` also contains a `legacy-print` action (PEVENT, software-timed pulse) intended as an A/B
  test against the TBP row path. It has no UI button or tests yet.
- Earlier same-day fixes in this area: `7ce87f2` (heater timer moved off TIMER1), `76124da` / `0663d70` (IRQ unmask and
  clock config), `e51db80` (bounded in-line pulse), `2bfc4de` (dropped the IR-capture bit, fixed the tick overflow).

## 6. What is not yet known

- Whether 50 ms+ means per row total or per event, and which build and limits were flashed for that test.
- Print darkness for values between 10 ms and 50 ms, and whether more than 50 ms is darker or burns the paper.
- `PHDIAG` output (`starts / isr / poll / over / cnt / con`) after a heated row on the current build.
- The true PB11 pulse width, from a scope or logic analyzer, for both the stock firmware and this one.
- The head temperature gate's behaviour during the long pulses (70 C start cutoff, 75 C abort).

## 7. Suggested next measurements (not done)

1. Scope PB11 on stock firmware during a normal print: pulse width, pulses per row, spacing.
2. Scope PB11 on this firmware for requested widths of 2, 10 and 30 ms and compare with the request.
3. Collect `PHDIAG` and `PDIAG` after a heated row.
4. Run the `legacy-print` A/B (software-timed PEVENT path) against the TBP row path at the same requested width.
5. Re-derive the stock timer clock from the TIMER2 setup against a measured width instead of the assumed 24 MHz.

## 8. Sprint 0 results: stock heater and step timing (2026-11-05)

Static work only (Ghidra export, vendor disassembly, the 2026-10-30 stock `app.bin` from `dumps/dump.bin`, SDK sources, and
the 2026-11-05 PTIME numbers). No hardware was touched and nothing was scoped. Tags: **MEASURED** = observed on hardware
(PTIME); **DERIVED** = read from code or arithmetic, one assumption stated; **OPEN** = needs a scope.

### 8.1 Stock TIMER2 setup (heater strobe)

Re-decoded from `drv_timer2_heat_strobe_init` (`0x1E09392`) and re-run in a script against the stock image
(`TIMER2->CON = 9 + 16*i`, `PRD = (144000 / table[i]) / 200`, first `i` with `144000 / table[i]` in 257..32766):

| Item | Value | Tag |
|---|---|---|
| Prescaler table (flash `0x1E1B8AC`) | 1, 4, 16, 64, 2, 8, 32, 128, 256, 1024, ... | read from the image |
| Chosen index / divider | 2 / 16 (144000/4 = 36000 is too big, 144000/16 = 9000 fits) | DERIVED |
| `CON` | `0x29`: counter on (bit 0), source `0b10` (bits 3:2), prescale index 2 (bits 7:4) | DERIVED |
| `PRD` | 9000 / 200 = 45 | DERIVED |
| Clock source `0b10` | SDK `timer_pwm_init` (`cpu/br23/mcpwm.c`) names it the 24 MHz crystal. Same CON bits 3:2 as `con_run` in `hal_time_test`, whose /256 row measured 23.98 MHz. | MEASURED (on TIMER3, not TIMER2) |
| ISR period | 45 x 16 / 24 MHz = **30.0 us**. If the counter period is PRD + 1 it is 30.67 us (+2.2 %); the /256 PTIME row cannot tell the two apart. | DERIVED |
| Meaning of `144000` | 24000 ticks/ms x 6 ms. The SDK timer helper in the same image builds its constants the same way (`24000 x 2` for the 2 ms system tick), which supports this reading. | DERIVED |
| ISR behaviour | Free-running (never stopped). Each tick: `window++`; PB11 high while `window < T` and `window < 200`; PB11 low otherwise. The latch step zeroes `window` and sets "latch pending". | DERIVED (hand decode, cross-checked against Ghidra) |

So the stock pulse is a counted number of 30 us ticks from a free-running timer, started at the latch with up to one tick
of start latency, and it ends by the ISR lowering PB11.

### 8.2 Stock heater pulse width (PB11 high, per print event)

Width = `(T - 1)` ticks for `1 <= T <= 200`, because the window counter is incremented before the compare. `T` is the sum
`A4 base + E/200` in normal mode (the other two components have no non-zero writer), or the `0xF2` direct value (capped at 200).

| Case | `T` | PB11 high per event | Per row (x 4 events) | Source | Tag |
|---|---|---|---|---|---|
| Minimum seen from a quality byte | 25 (`0xA4` `0x31`, no energy) | 0.72 ms | 2.9 ms | `re/docs/printhead-protocol-and-heating.md` 8.2; table constant `0x46351E19` | DERIVED |
| Typical, quality only | 30..75 (`0x32`..`0x35`) | 0.87..2.22 ms | 3.5..8.9 ms | same | DERIVED |
| Typical, quality `0x32` + energy 6000 | 60 | 1.77 ms | 7.1 ms | same | DERIVED |
| Normal-mode maximum | 75 + 100 = 175 (quality max + clamped energy) | 5.22 ms | 20.9 ms | `0xAF` clamp to 100 | DERIVED |
| Hard cap (`0xF2` direct = 200, or any `T >= 200`) | 200 | 199 ticks = 5.97 ms | 23.9 ms | window reset at 200 in the ISR | DERIVED |

All widths scale by 30.67 / 30.0 if the PRD + 1 reading is right.

### 8.3 Is 50 ms consistent with stock?

No, not on pulse width alone. Taking the owner's "50 ms" as the per-row total (about 12.5 ms per event, still an
assumption, see section 6), it is 5.6x the longest quality-only stock pulse (2.22 ms), 2.4x the normal-mode maximum
(5.22 ms) and 2.1x the stock hard cap (5.97 ms). If it were per event it would be 8.4x the cap.

The 30 us tick explanation (hypothesis 1 in section 4) is now much weaker: the stock timer uses the same clock source bits
and the same prescaler table as our TIMER3, and that source measured 23.98 MHz. A wrong tick would have to be 5x or more, and
the only slack found is the +2.2 % PRD + 1 question. It is not eliminated until PB11 is scoped on stock.

What differs between stock and Pinbridge, by code reading (candidates, none confirmed; ordered by how cheap each is to test):

1. **Stock re-shifts and re-latches the same 48 bytes before every one of the four pulses.** Pinbridge shifts and
   latches once per row and then fires the slices (`pb_core.h` stage 3). Whether the head loses or needs the latched data
   between pulses is unknown. Test: a row variant that re-latches before each slice.
2. **Phase offset.** Stock starts the heat window at the latch and advances the motor phase straight after it, so the
   pulse overlaps the coil change. Pinbridge starts slice `i` only after motor edge `i` has completed. Supply sag or
   timing relative to the coil step could change the energy actually delivered.
3. **Stock busy-delay units.** Stock `drv_busy_delay(n)` counts loop iterations (PB11 low guard 20, data setup 2, clock
   high 1, latch low 10, post-latch 1). None of these are microseconds, and the CPU clock is not recovered, so the real
   stock latch and clock widths are unknown. Pinbridge uses 2 / 1 / 2 us and a 10 us latch low with a 100 us guard
   (and `hal_delay_us` is not accurate, see `timer-architecture-and-usage.md` section 3).
4. **Pulse width actually delivered by this firmware** at a requested width. PTIME makes a TIMER3 error unlikely, but
   nothing has scoped PB11 yet. This is the Sprint 3 measurement.
5. Head supply voltage under load, head temperature and dot density (section 4, hypothesis 5). Not investigated.

### 8.4 Stock step cadence

| Item | Value | Tag |
|---|---|---|
| System timer hardware | TIMER1, `CON 0x19` (24 MHz crystal, /4), `PRD 12000`: **2.000 ms, fixed**. `drv_timer1_update_period` multiplies 24000 by a constant 2, whatever period a client asked for. | DERIVED |
| PTIME: `sys_hi_timer_add(..., 1)` | 2.000 ms per callback | MEASURED |
| Meaning | A request below 2 ms is rounded up to one 2 ms system tick. The "1 ms" floor in Pinbridge never existed. | DERIVED, fits the measurement |
| Stock callback `app_2ms_adc_and_print_tick` | `usr_timer_add(..., 2)`; the SDK stores the expiry as `jiffies_msec + period`, so the unit is ms, and the expected cadence is 2 ms. Whether an expiry that lands exactly on a tick fires on that tick or the next (2 ms vs 4 ms) was not traced. | DERIVED / OPEN |
| Event post rate | every `g_print_tick_div` callbacks, clamped to at least 3: 6 ms minimum per print event (12 ms if the callback were really 4 ms). `0xA4` writes divider 3. | DERIVED |
| Events per row | 4. Each event: PB11 low, shift 384 bits, latch, advance one of 8 half-step phases. Row minimum about 24 ms. | DERIVED |
| Heat gating | Stock does not gate heat on motor edges. The window opens at the latch of the same event and closes within 199 ticks (5.97 ms), which is inside the 6 ms minimum event period; the next event clears PB11 before shifting. | DERIVED |
| Phase settle | Each phase routine ends in a 1000-iteration busy loop (not 1000 us). | DERIVED |

### 8.5 Reshift, relatch and per-event data

Confirmed from the `0x122` event handler in `decompiled.c`: the row is copied into four buffers
(`g_head_line_buf0..3`), and every event calls `drv_head_shift_latch_step` on one of them, so the head is reshifted and
relatched four times per row, and the four events carry the same row data. Correction to `re/docs/printhead-protocol-and-heating.md`
section 7: the event that loads the row also shifts and latches `buf0` (the load path falls through to the shift call), so a row takes
4 events, not 5. `re/` is read-only and was not edited.

### 8.6 Scope capture plan (needs the owner; nothing run)

Probes: PB11 (strobe), PB8 (latch), one motor phase pin (PA9, PA10, PC3 or PC2), and optionally PB9 (clock).
Stock firmware, a solid-black row pattern, three captures: `0xA4` `0x31`, `0x33` and `0x35` with no `0xAF` energy.

Record per capture: PB11 high width, pulses per row (expect 4), spacing between pulses (expect at least 6 ms), offset from
the PB8 rising edge to the PB11 rising edge (expect under 30 us), offset from the motor edge to PB11, PB8 low width, and the
length of the PB9 burst.

Pass: widths within 5 % of (T - 1) x 30 us (0.72, 1.56, 2.22 ms) confirms the 30 us tick and closes hypothesis 1. A width
more than a few percent away from these points to a different tick; a PRD + 1 reading shows as +2.2 %.
Then repeat on the Pinbridge build for requested widths of 2, 10 and 30 ms (section 7, item 2).

Not done in Sprint 0: any hardware capture, and the 2 ms vs 4 ms callback boundary question.
