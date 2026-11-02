# Thermal G-code: Protocol Design Doc

*Working title. A G-code-style protocol for a 384-dot thermal printer with true grayscale and printer-side calibration.*

---

## 1. Goals

- A **generic, G-code-like text protocol** that any compatible printer can accept, regardless of its head or motor.
- **Explicit control** of the paper stepper and the heat head, with an **opt-in shortcut** ("send row, it prints and advances") for simple use.
- **Real grayscale**, not dithering: multiple timed burn passes per row.
- **Printer owns the physics.** The host sends *intent* (gray levels); the printer converts them to burn times using its own stored calibration.
- **Calibration math lives on the PC** (photo analysis, curve fitting). The result is pushed to the printer as commands and stored in flash, so the printer works standalone afterward.
- **Safety in firmware**, never in the host.

Design lineage: Marlin (command set, `ok` flow control, `M500` family, checksums) plus Klipper's philosophy (host does the heavy math, firmware executes precisely).

---

## 2. Target Hardware

| # | Component | Notes |
|---|-----------|-------|
| 1 | Paper stepper (single axis) | Called axis **Y** in the protocol |
| 2 | LED | Status / blink / brightness |
| 3 | Optical paper sensor | Paper presence and edge detection |
| 4 | Power button | |
| 5 | 384-dot thermal head | Data / clock / strobe (latch) pins |
| 6 | Heat-enable pin | Master enable for the head's heater supply |
| 7 | Battery level pin | ADC measurement, reported in mV |

---

## 3. Protocol Basics

- Plain ASCII text, one command per line, terminated by `\n`.
- Comments start with `;` and run to end of line.
- Commands are case-insensitive; hex payloads are uppercase by convention.
- **Replies:** the printer answers every line with:
  - `ok` on success
  - `ok <data>` for queries (e.g. `ok V3912`)
  - `err <code>` on failure (e.g. `err nopaper`, `err lowbat`, `err range`, `err calcrc`)
- `ok` acts as **flow control**: the host sends the next line only after receiving `ok` (or keeps a small window of in-flight lines if the printer advertises a buffer depth in `M115`).
- **Optional line checksum** (Marlin style): `N12 <command> *57`. Recommended over BLE or noisy serial.

---

## 4. Row Data

A row is **data**. Printing it is a **separate, explicit action**, unless the opt-in shortcut is used.

### 4.1 Binary rows (1 bit per dot)

| Command | Meaning |
|---------|---------|
| `R<96 hex>` | Load 48 bytes (384 bits) into the line buffer. MSB = leftmost dot. |
| `P<96 hex>` | **Opt-in shortcut.** Load the row, burn it, then advance by the configured steps per row. |

### 4.2 Gray rows (4 bits per dot)

| Command | Meaning |
|---------|---------|
| `Q<384 hex>` | Load a gray row: 384 dots x 4 bits = 192 bytes = 384 hex chars. |
| `QP<384 hex>` | Opt-in load, burn, and advance version of `Q`. |

**Gray level convention:** `0` = white (no burn), `15` = maximum black. Levels are **perceptually even**: the printer's LUT maps them to burn times so that equal steps look equal.

`M205 S<1|2|4|8>` sets the default gray depth in bits per dot. Depth 1 behaves exactly like `R`/`P`.

### 4.3 Optional extension

An `R`/`Q` variant with **run-length encoding**, since text and graphics rows are mostly zeros. Not required for v1.

---

## 5. Motion Commands (G-codes)

Y axis = paper feed. Positive = forward, negative = reverse.

| Command | Meaning |
|---------|---------|
| `G0 Y<n> F<steps/s>` | Feed `n` steps with **no heating** |
| `G1 Y<n> F<steps/s>` | Step `n` steps **while burning the buffered row** (explicit stepping) |
| `G4 P<ms>` | Dwell (e.g. let the head cool) |
| `G28` | Feed until the optical sensor detects the paper edge, then zero Y |
| `G92 Y0` | Reset the position counter |

`G1` burns whatever is in the buffer, binary or gray, so stepper logic is identical for both.

---

## 6. M-codes

### 6.1 Device control

| Command | Meaning |
|---------|---------|
| `M17` / `M18` | Stepper on / off |
| `M81` | Power off |
| `M115` | Firmware info: version, dot count (384), capabilities, buffer depth, last calibration metadata |
| `M120` | Read paper sensor |
| `M125` | Read battery (reply in mV) |
| `M150 S<0-255>` | LED brightness |
| `M150 B<n>` | LED blink |

### 6.2 Printing parameters

| Command | Meaning |
|---------|---------|
| `M200 S<us>` | Burn time per strobe (darkness control for binary rows) |
| `M201 S<n>` | Dots per strobe group |
| `M202 S<n>` | Steps per row for `P`/`QP` auto-advance (`0` disables it) |
| `M203 S<n>` | Max burn time cap |
| `M204 S<0\|1>` | Auto-compensate burn time using battery voltage |
| `M205 S<1\|2\|4\|8>` | Gray depth, bits per dot |
| `M207 I<level> S<us>` | Burn time for one gray level (manual LUT entry) |
| `M208 S<strength>` | Heat-history compensation strength (shaves burn time after dark rows) |

### 6.3 Calibration and profile

See section 9.

### 6.4 Example (simple mode)

```
M202 S2          ; 2 steps per row
M200 S900        ; 900 us burn
G28              ; find paper edge
P00FF00FF...     ; print row, auto-advance 2 steps
P...
G0 Y40           ; feed paper out
```

---

## 7. Grayscale Rendering

Each dot is only on or off at any instant, so gray is produced by **multiple strobes per row at different energies**.

### 7.1 Binary-weighted passes (preferred)

For 4-bit gray: 4 passes with burn times of **1x, 2x, 4x, 8x** a base unit. A dot fires in pass *k* if bit *k* of its value is set. 16 levels cost only 4 strobes.

*Thermometer coding* (fire if value >= pass number) needs 15 passes and is slower; not recommended.

With a calibrated LUT, the actual burn time for each level comes from the table, not a pure linear multiple, so a pass-based scheme may be replaced by "one pass per distinct LUT time". The firmware decides; the protocol is unchanged.

### 7.2 Rules for every pass

- Each pass goes through the strobe-group splitting (`M201`) so peak current stays safe.
- Insert a **short cooling gap** between passes, since the head's thermal mass smears energy between them.

### 7.3 Known pitfalls

- **Thermal paper is very nonlinear.** Linear burn times give ugly ramps. The per-level LUT and a printed step wedge are essential.
- **Heat accumulates along the paper.** A dark row followed by a light row prints too dark. Track the previous row's energy and reduce the burn time (`M208`).
- **Battery voltage affects energy.** Scale burn times with voltage (`M204`).
- **Gray rows are 4x the data.** Buffer a few rows on the printer and rely on `ok` flow control.

---

## 8. Safety (Firmware-Enforced)

These must hold **regardless of host behavior or calibration values**:

- **Hard burn-time cap.** Any computed burn time above the firmware maximum is clamped. A bad calibration yields an ugly print, not a scorched head.
- **Host watchdog.** Drop heat-enable if no command arrives for N ms. A stalled host mid-row must not leave the head hot against stationary paper.
- **Refuse to burn** if the battery is low or no paper is detected (`err lowbat`, `err nopaper`).
- **Strobe splitting** so no more than the allowed number of dots fire simultaneously.
- **Conservative baked-in defaults** so an uncalibrated printer still prints (lighter, but safely).
- **Profile validation**: checksums, version, and hardware ID checks (see section 9.3).

---

## 9. Calibration System

### 9.1 Principle

> The print protocol speaks in **intent**. The printer owns the **physics**.

- **PC app:** prints the calibration target, analyzes a photo/scan of it, fits curves, and emits calibration commands.
- **Printer:** stores the values in flash and applies them on every print. No PC needed afterward.
- A different printer with different hardware accepts the exact same print file.

### 9.2 Calibration commands

| Command | Meaning |
|---------|---------|
| `M209 T0 V<hex>` | **Gray LUT**: 16 levels x 16-bit µs (64 hex chars) |
| `M209 T1 A<n> B<n>` | **Voltage compensation**: reference mV and slope |
| `M209 T2 A<n> B<n>` | **Thermal model**: decay constant and carryover gain |
| `M209 T3 V<hex>` | **Per-dot gain map**: 384 bytes (or per-segment for a smaller map) |
| `M209 T9 D<date> P<name>` | **Metadata**: calibration date and paper name, reported via `M115` |
| `M210 P<slot>` | Select a **paper profile** (different papers need different tables) |
| `M500` | Save profile to flash |
| `M501` | Load profile from flash |
| `M502` | Reset to firmware defaults |
| `M503` | Print the current profile (for read-back verification) |

### 9.3 Profile integrity

- Every `M209` line ends with a **checksum**; bad ones are rejected with `err calcrc`.
- The stored profile includes a **format version** and a **hardware ID**, so a profile from one head cannot be loaded onto another.
- Values are **range-checked and clamped** against firmware limits on load.

### 9.4 Calibration target (what the printer prints)

- **Corner markers** (ArUco or QR-style) for detection and perspective correction.
- **Reference patches:** unburned paper (white) and maximum burn (black), to normalize lighting and exposure.
- **Step wedge:** burn time sweeping 0 to max at a fixed battery voltage. Gives the density curve.
- **Cooling test:** same burn with varying gaps between passes. Fits the head's thermal time constant.
- **History test:** a dark row followed by a light patch. Measures heat carryover error.
- **Width stripes:** the same burn across all 384 dots. Detects uneven dots for the per-dot/segment gain map.
- **Voltage tests:** repeat patches at different battery levels, with the printer reporting mV for each patch.

### 9.5 PC-side processing

1. Detect the markers and **warp** the photo to a flat, known layout.
2. **Normalize** using the white and black reference patches.
3. Convert to **linear luminance**, then fit a **monotonic curve** (burn µs to density).
4. **Invert** the curve to get 16 burn times that are *perceptually* evenly spaced (equal steps in L*, not linear).
5. Fit a **first-order thermal model** (exponential heat decay) from the cooling and history tests.
6. Fit a **voltage scale factor** from the voltage tests.
7. Build the per-dot gain map from the stripe test.

A flatbed scanner gives far more consistent results than a phone photo. If using a phone, fix the lighting and lock the exposure.

### 9.6 Runtime model (on the printer)

```
burn_time(dot) = LUT[target_level]
               x heat_state_correction        (from thermal model + previous row energy)
               x voltage_correction           (from battery mV)
               x dot_gain[dot]                (from gain map)
               -> clamped to max burn cap
```

The firmware tracks the heat state per row. Alternatively the host can precompute everything, but the intended design keeps this on the printer.

### 9.7 Workflow

1. Host sends `M502` (reset to defaults).
2. Host prints the calibration target.
3. User photographs/scans it and uploads it to the PC app.
4. App fits all parameters.
5. App sends the `M209` lines, then `M500` (save).
6. App sends `M503` and compares the read-back to verify storage.

---

## 10. Firmware Notes

- **Split strobes:** firing all 384 dots at once usually exceeds what a battery or head supply can deliver. Group dots (`M201`) and strobe sequentially, transparently to the protocol.
- **Stepper and burn timing** must be interleaved carefully: burn time per row must fit inside the step interval at the requested feed speed, or the printer should slow the feed and report it.
- **Buffering:** keep a small ring buffer of rows so gray data (4x size) streams smoothly.
- **Heat-enable discipline:** raise it only for the duration of a strobe sequence, drop it immediately after.

---

## 11. Open Questions / Next Steps

- Final **binary profile layout** and **CRC scheme** (CRC-16 vs CRC-32; line checksum vs payload checksum).
- Exact **JSON profile format** on the PC side (mirrors the `M209` fields, plus raw calibration data for re-fitting).
- Pick the transport: USB serial, BLE, or both (affects buffer depth and checksum requirements).
- Decide whether gray passes are fixed binary-weighted or LUT-driven (one pass per distinct time).
- Reference implementation: **pass-scheduling loop in C** and **strobe-group logic**.
- Design the **calibration target layout** in detail (marker positions, patch sizes, paper length budget).
- Decide on RLE row compression and its syntax.
