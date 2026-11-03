# Bench findings 2026-11-03 (second session): comms robustness, step timing, boot wiggle, temperature, heater

Status: **in progress. Host robustness step 1 and firmware watchdog policy (step 2 / F3) have software regression coverage; remaining items are open.** Follow-up to
[2026-11-03-move-wedge-and-watchdog-cancel.md](2026-11-03-move-wedge-and-watchdog-cancel.md).

Evidence types are labelled throughout: **[trace]** = the owner's `--debug` host log from the printer on
2026-11-03 12:23-12:25 (local), **[source]** = read from the code at `2295627`, **[hyp]** = hypothesis,
**[owner]** = stated by the owner, not measured by us. Nothing here is a hardware measurement by the agent.

Firmware under test: the CI artifact `pinbridge-br23-41608188029` (commit `2295627`) as far as we know.
The device reports `build_id = 0`, so the trace itself cannot prove which build ran (see item 12).

## 0. Owner requirements (the "why")

1. The printer must **always** answer on the link while power is on. Nothing, including a full
   command window, a long job, a heater wait or a wedged job, may make it unreachable.
2. The host must **never drop the link by itself**. It may only reconnect after a real transport loss.
3. Boot wiggle: a short pause before reversing, push **5 mm** of paper out and back in, and **no wiggle
   when there is no paper**.
4. Temperature must be shown in degrees C, not raw ADC counts.
5. Find out whether the heater works at all. A full-line print with 5000 us heat showed no color.
6. Speed and distance in the UI must be correct (mm, mm/s), and the half-step/full-step maths must be right.

## 1. Findings from the trace

### 1.1 What worked [trace]
- Four MOVEs at exactly 1000 us (ids 6, 8, 9, 11 of the second session) all reached DONE; none showed the
  repeating `PROGRESS remaining=0` signature of the old wedge. That is a small sample for a probabilistic
  race, so it is encouraging, not proof.
- No `!wdg` line, no `ERR NOSESSION` in this trace. (The watchdog is not fixed yet; it was just not
  triggered.)
- STAT bodies decode sensibly: paper present = 1, battery about 4156 mV, head temp raw about 0x172 (370).

### 1.2 Steps take about 2x longer than the firmware says [trace + source]

| Request (id) | Period the firmware reports as effective (DONE body) | Expected | Observed |
|---|---|---|---|
| +480 steps @ 1000 us (id 6) | 1000 | 0.48 s | 0.97 s |
| -480 @ 1042 us (id 24) | 2000 | 0.96 s | 1.93 s |
| +160 @ 1563 us (id 41) | 2000 | 0.32 s | 0.65 s |
| +480 @ 2083 us (id 33) | 3000 | 1.44 s | 2.89 s |
| +480 @ 3125 us (id 17) | 4000 | 1.92 s | 3.85 s |

PROGRESS on id 17 agrees independently of host timing: remaining steps went 357, 232, 107 (about 125 per
second) where 250 per second was expected. The 80-step MOVE in the first bug report (167 ms) was also about
2x too slow; that report called it "normal".

Source: `pb_step_tick` (`pb_printer.h`) advances exactly one phase per `step_dwell_ms` ticks (gap is 0 on the
TBP path, `pb_step_start(..., gap_us=0, ...)`). The host-side C simulation completes 80 steps in 80 ticks.
So the firmware logic is "one phase per period" and the extra factor of 2 is most likely the **real tick
rate of the timer**: `sys_s_hi_timer_add(NULL, pb_step_timer, 1)` (`app.c:295`) probably fires every ~2 ms,
not 1 ms [hyp]. The exact ratio (2.00x for every period) fits a fixed tick, not a fixed per-step gap.

Consequences if confirmed:
- The UI's mm/s is **half the real speed** (31.25 mm/s on screen is about 15.6 mm/s in reality).
- 1000 us is really 2000 us. The "minimum period 1000 us" and the effective period reported in DONE are wrong.
- ROW duration estimates (`pb_core.h` `duration = steps * period / 1000`) and the host MOVE/ROW timeouts
  are too short by 2x for the same reason. Timeouts only just survive today.
- The wedge race analysis still holds logically (it depends on "wait counter == 1"), but the real time
  window is 2 ms.
- Distances are **not** affected (they count steps).

To do: read the SDK hi-timer implementation (read-only), or measure: one MOVE of 1000 steps at 1000 us,
timed on the bench. Then either make the tick truly 1 ms, or set the real floor (probably 2000 us), correct the
reported effective period, the UI speed limits and the timeouts.

### 1.3 COILS release is rejected as BUSY and the host then drops the link [trace + source]
- `ERR 0x09` is BUSY (spec table, line 397). COILS is idle-only (spec line 324).
- The host sends a COILS release after each MOVE when `releaseAfter` is set (`server.ts:627`). With another
  MOVE still queued the firmware answers BUSY (ids 4, 10, 19, 55 in the trace).
- If nothing is waiting for credit the host just retries after the next DONE (works). If the window is full
  it calls `drop("TBP command rejected while commands are waiting for credit")` (`server.ts:404`), tears the
  link down and reconnects (26.086, 39.240, 47.686). Each reconnect costs 2-3+ seconds.
- The firmware already releases the coils at idle (`pb_coils_idle`, idle hold 0 by default), so the explicit
  host COILS release is redundant for the default policy.

### 1.4 HELLO replies lost after a drop [trace, cause unknown]
After two of the three drops, attempt 1 of HELLO got **no reply** and attempt 2 got only the **tail** of the
INFO frame (`RX? "A600BB80...8BF3" Invalid TBP hex frame`); attempt 3 worked (about 3.2 s lost each time). The
start of the reply is missing: either the device lost leading TX bytes, or the host UART layer started reading
mid-line or discarded a partial line. Open questions: is the old queue still running when HELLO arrives, does
HELLO cancel it, and is there leftover RX/TX buffer state across a host-side reconnect? Needs `--debug` plus a
raw byte capture around the drop.

### 1.5 `UART closed by device` at 12:23:27.757 [trace, cause unknown]
Happened during idle STAT polling with no motion, so it is not the wedge. The device answered again 3.4 s
later without a button hold. This fits a USB-side drop better than a power loss, but it is unproven. Note
that after an MCU reset `power_ready` is 0 again: the LED stays dark, the boot wiggle does not run, and
`pb_power_led_poll` releases PA1 about 1 s after boot unless the button is held. Watching the LED and the
wiggle when it happens would tell reset from USB glitch. Same open question as bug 1, section 4.2 (check
`estop_fast_requested` after a detach, `app.c`).

## 2. Requirement: communication must never block (design to implement)

### 2.1 What can block communication today [source]
1. The legacy 5 s watchdog (`wdg = 5000`, `pb_core.h:339`, trip at `:1675`) cancels all work, prints raw `!wdg`
   and sets `tbp_active = 0`. (Bug 1, section 3.2.)
2. A wedged job (the 1000 us race, fixed in `86dc116`) stops the FIFO; other classes of wedge may exist.
3. Idle-only commands (COILS) give `ERR BUSY` while work is queued.
4. Host credit: with a full window of 4 the host cannot send STAT/ESTOP, and the keepalive is skipped while
   anything is pending (`server.ts:95`). STAT and ESTOP are *immediate* commands that do not touch the
   queue (spec line 172), so the host's credit rule is stricter than the protocol.
5. The host closes the link on protocol-level problems (`drop()` callers: `server.ts:362, 370, 376, 405, 525, 537`,
   keepalive failure `:108`).
6. Firmware busy-waits (`pb_wait_us`) run inside `pb_poll`. Each is short today (a 48-bit shift chunk is
   well under 1 ms, a heat slice is at most `PB_TUNE_HEATER_SLICE_MAX_US`), but this has not been audited
   against the USB RX ring (`usb_rx_wr`, `rx_overflow`) for a full window of 119-char ROW lines.

### 2.2 Firmware requirements
- F1. **Control plane never blocked.** STAT, HELLO, ESTOP, CONFIG, FLUSH are handled in the RX path no matter the
  queue state: never QFULL/BUSY for them, never delayed by HOT wait, DUTY wait, DWELL or a ROW burn beyond a
  documented bound.
- F2. **Defined response bound.** Every well-formed frame gets ACK or ERR within a stated time (for example 50 ms),
  including during a heater wait. Add transcript/ISR tests that send STAT in every executor stage.
- F3. **One link policy instead of the legacy watchdog.** An active TBP session with queued/running work is never
  killed by silence. A genuinely silent host triggers a configurable link timeout that puts motion/heater in
  the safe state but **keeps the session and keeps answering**, and reports it with a TBP EVENT or a defined
  cancel reason. No raw `!wdg` text on the wire.
- F4. **Make COILS never BUSY-fatal.** Either queue it like other commands or accept it as a no-op when work is
  pending. Update the spec and matrix.
- F5. **Detect a stuck executor.** The terminal-state defensive path from the wedge plan (a train that ended with an
  unreachable target terminates the command) plus a progress watchdog on the executor stage, so any future wedge
  becomes a FAILED status rather than silence. Consider the hardware watchdog only after checking the
  PA1 power-latch behaviour on reset (section 1.5).
- F6. **RX ring and resync.** Verify the ring holds a full window plus STAT; on overflow or garbage, resync on the
  next newline instead of wedging (relates to 1.4).

### 2.3 Host requirements ("never drops by itself")
- H1. `drop()` and port close happen **only** for transport loss (device closed, IO error). Timeouts, `ERR`,
  sequence mismatch, BUSY, QFULL, and credit starvation are handled by resync (STAT, or HELLO on the same open
  port) and retry, never by closing the port. After transport loss: reconnect forever with short backoff.
- H2. **Reserve control credit.** Send STAT keepalive regardless of pending commands, and keep one window slot
  free for STAT/ESTOP (use `window = depth - 1` for data commands, or let immediate commands bypass credit).
- H3. Do not send idle-only commands while work is pending. Remove the redundant `releaseAfter` COILS when the
  firmware idle policy already releases, or wait for an empty queue first. Treat BUSY as "retry later".
- H4. HELLO recovery: discard any partial RX line before each attempt and retry faster than 1.6 s.
- H5. Show raw non-TBP lines and every reconnect reason in the UI event log (bug 1, 3.4).
- Tests: a fake transport that injects BUSY, QFULL, truncated frames and silence must show the port is never
  closed and the session recovers.

## 3. Half-steps, full steps and the calibration question [source + owner]
- The firmware sequence is the eight-state half-step table (`PSTEP`, journal 2026-11-02). One entry = one
  half-step = one phase change; **8 half-steps = one electrical cycle = 4 full steps.**
- For a motor with 200 full steps per revolution: 400 half-steps per revolution, i.e. 50 loops of 8, which is
  **200/4** loops, as the owner worked out. 200/8 would be 25 loops (half a turn). The motor's real steps per
  revolution and the gear ratio are unknown to the agent: unverified.
- This does **not** explain the 2x speed error: speed is time per phase change, and the firmware does one phase
  change per period (section 1.2). It is a separate risk about **distance**.
- `STEPS_PER_MM = 32` (`host/pinbridge/bun/units.ts`) assumes "160 steps = 5 mm" means **half-steps**. If the 160
  were full steps it is 64 half-steps/mm and every distance is 2x too short. Measure: send a known number of
  half-steps and measure the paper with a ruler, then fix `STEPS_PER_MM` and the docs.
- Boot wiggle today is `PB_TUNE_BOOT_WIGGLE_STEPS = 200` half-steps = 6.25 mm at 32/mm, not 5 mm.

### Step 4 implementation note (distance calibration contract)

Added `PB_TUNE_STEPS_PER_MM` beside the firmware motor tuning and a host source-contract test that keeps it equal to `STEPS_PER_MM`. Both remain 32 half-steps/mm based on the owner's stated 160 half-steps = 5 mm. This is **not a completed physical calibration**: a ruler measurement is explicitly pending for the owner's post-software hardware pass; if it differs, update both constants and rerun conversion tests.

## 4. Boot wiggle [source]
Current behavior (`pb_boot_wiggle_poll`, `pb_core.h:533`): forward 200 half-steps, then **immediately** back
200 (stage 1 to 2 with no pause, `pb_step_start` called in the same poll), on power-on, with no paper check.
Required:
- W1. A short pause (a few tens of ms, a tuning constant) with coils held between the forward and the reverse leg,
  and again before releasing the coils. The owner reports the instant direction change makes the gears act weird.
- W2. Distance 5 mm out and 5 mm back = 160 half-steps at the owner's calibration (confirm calibration, section 3).
  Define it in mm in `pb_tuning.h` and derive the steps.
- W3. **Skip the wiggle when no paper is present.** The paper sensor is PB3, present when the pin reads low
  (`PB_TUNE_PAPER_PRESENT_LEVEL`). `pb_sensors_sample` reads it directly with no debounce; check it is valid and
  stable at the moment the power hold completes.
- W4. Confirm which direction is "out" (reverse flag 0 = forward) on the real mechanism.
- Test: extend a C transcript/sim test (tick loop with a paper pin) for pause, distance and the no-paper skip.

### Step 5 implementation note (boot wiggle)

The wiggle now derives 160 half-steps from `PB_TUNE_BOOT_WIGGLE_MM=5` and the shared 32 half-steps/mm assumption. It requires a valid paper-present reading plus two more present samples 10 ms apart before moving; absent/invalid or unstable paper skips the wiggle. The out leg is followed by a 30 ms coil-held pause, then the return leg, then another 30 ms coil-held pause before release. C transcript-side simulation checks exact step count/net-zero phase, both pauses and coil hold, cancellation, and no-paper skip. The outward flag is provisionally forward (`reverse=0`); the actual physical direction and distance calibration remain owner hardware checks after software tasks. No physical behavior is claimed.

## 5. Temperature shows raw counts [source]
- STAT/INFO carry `head_temp_raw` in ADC counts by design (spec lines 242, 266). The UI prints "raw"
  (`app.js:183`). Firmware already converts to degrees C for the heat gate (`pb_sensor_head_temp_c`, the stock
  101-entry table in `pb_thermistor_table.h`, index i = i-20 degC) but never sends it.
- Estimate only (beta model, not the stock table): raw 370 is about 24 C, which looks like room temperature;
  the raw value drifted 0x172 to 0x16C over about 35 s in the trace.
- Fix options: (a) host converts raw to C with the same table (generated from `pb_thermistor_table.h`, with a
  consistency check like the generated pins check) and keeps raw as a tooltip; (b) add a `temp_c` field to STAT in
  a spec revision. (a) is cheaper and needs no firmware change. Also show the gate limits in C (cutoff 70, abort 75,
  resume 50).
- Voltage reading is accepted by the owner as accurate enough (about 4.16 V in the trace); still to be checked
  against a meter, as `pb_tuning.h` says.

### Step 6 implementation note (Celsius display)

The host now converts `headTempRaw` with a generated TypeScript copy of the same stock table used by `pb_sensor_head_temp_c`. `scripts/check-pinbridge.sh` regenerates and compares that table against `pb_thermistor_table.h`, and unit tests cover raw 370 -> 24 °C, raw 412 -> 20 °C, a table boundary, and short/open/sentinel inputs. The UI displays °C, keeps the raw ADC count in a tooltip/footnote, and shows the 70 °C cutoff, 75 °C abort, and 50 °C resume limits. Invalid readings remain unavailable rather than being guessed. This is software parity evidence; physical sensor calibration remains unverified.

## 6. Does the heater work? [source + owner, hardware unverified]
The owner's full-line print (UI "print full line", 5000 us heat) showed no visible color. **No ROW frame appears in
the trace above**, so what the firmware answered to that print is unknown; capture it with `--debug`.

Findings from the source:
1. **The UI always sends `slices: 1`** (`app.js` print button; `server.ts` `print-solid` default). The firmware limits one
   heater pulse to `PB_TUNE_HEATER_SLICE_MAX_US = 2500`, and `ROW` rejects `ceil(burn / slices) > 2500` with
   `ERR RANGE arg 48` (`pb_core.h` ~1468). So 5000 us with slices 1 should be **rejected**, not printed. The UI
   max is `burnMaxUs` (10000, a per-row total), which is misleading: the usable maximum with 1 slice is 2500.
   (If the owner saw it "attempt" a print, either the burn was lower, or an earlier build was running.)
2. Even an accepted single 2500 us pulse is about 1/4 of the stock energy at best quality. Stock fires **4 events
   per row at 0.72-2.22 ms each** (about 2.9-8.9 ms per row). To match it the UI should send `slices = 4` (needs
   `|steps| >= 4`, the default row is 8 half-steps) and a burn of 4 x 0.7..2.2 ms, within `PB_TUNE_HEATER_BURN_MAX_US` = 10000.
3. A ROW only heats when the gate passes: valid temp, battery at or above 3400 mV, paper present, temp below 70 C. The trace shows
   those inputs are fine (temp about 24 C, 4156 mV, paper present), so the gate should pass.
4. The row path is shift (PB10/PB9) then latch (PB8 low/high) then PB11 high for `on` us (`pb_core.h` ~1085-1140). Heater
   polarity, latch polarity, the pin map and the timing of `pb_wait_us` are **not validated on hardware**; the owner
   enabled the heater on stock-derived numbers (`pb_tuning.h` header says so). The journal notes the old delay loop
   overstated pulse times about 10x before it was recalibrated.
5. Other causes unrelated to firmware: paper not thermal, or loaded the wrong way, head not seated, battery sag during
   a burn while the motor runs.
- Diagnostic order (owner, bench, `--debug`): (1) send a ROW with burn 2000, slices 1, steps 0 and read the reply: ERR or
  DONE? (2) with a multimeter/scope on PB11 or the head supply, confirm the pulse width and that PB11 really toggles;
  (3) compare against stock firmware on the same paper; (4) then slices 4 with realistic burn. Report evidence types separately.

## 7. Other items
- 12. **Build id.** `build_id = 0` in HELLO. The owner uses the CI artifact only. Cheap fix: have CI pass the git SHA
  (or the run id) into the build so HELLO reports it, and name the artifact with the SHA. Low priority.
- **Still open from the original brief:** the 5 s watchdog (section 2.2 F3), host event log for raw lines, keepalive with a
  full window, docs updates (Sprint 4/6 handoff, journal, resolution note in the first bug file).
- **Unexplained cancel at 10:51:42** (bug 1, section 2.2/4.1) is still open.

### Step 7 implementation note (heater slicing UI)

Heated `print-solid` rows now use four slices by default; each slice is validated against the firmware's 2500 µs cap, while the input remains total row burn (maximum 10000 µs across four slices). The WebUI displays the per-slice ceiling, total-row ceiling, and four-slice behavior, and refuses a heated row with fewer than four feed half-steps. The encoder and server tests prove 8000 µs total becomes four 2000 µs slices and reject totals that exceed the per-slice ceiling. Burn remains zero by default and the UI remains gated on firmware sensor/safety capabilities. The hardware diagnostic sequence below remains owner-run; software tests do not establish heater polarity, pulse width, thermal-paper output, or stock-equivalent energy.

### Step 8 / F5 implementation note (executor stall detection)

Added a 900 ms progress watchdog for non-timed executor stages. It observes stage changes, row shift bits/slices, completed steps and remaining steps; repeated PROGRESS messages alone do not reset it. DWELL, HOT_WAIT and DUTY_WAIT retain their own deadlines and are excluded. On a stall it reports FAILED/INTERNAL (`0x1F`, arg `1`), turns the heater off, cancels queued work and returns the motor to its safe state. `tbp-executor-stall-move.txt` and `tbp-executor-stall-row.txt` simulate a dead step-timer ISR while the command task still polls; both failed before the watchdog and pass after it. Portable C simulation only; no firmware build, CI, flash or hardware test.

### Step 8 / F6 implementation note (RX resync and HELLO recovery)

On firmware RX-ring overflow, the adapter now clears the ring and both partial-line scanners enter discard-until-newline mode, preventing a tail fragment from being parsed as a new frame. `tbp-rx-resync.txt` simulates that state, discards the first truncated line, and verifies the next complete STAT works. On the host, `Uart.discardPartialLine()` clears only an incomplete buffered line; it is called before every HELLO send/retry. HELLO uses 400 ms transaction attempts plus 100 ms backoff and retries on the same open UART through timeouts or malformed INFO instead of closing the port. Fake-UART and static contract tests cover this recovery policy. These are host/C tests; physical USB overflow/recovery still needs validation.

### Step 8 / build ID implementation note

CI passes `github.sha` into `scripts/build-app.sh`; the applied firmware header receives `PB_BUILD_ID` from the first eight SHA hex digits, and HELLO writes that u32 into INFO. `BUILD-INFO.txt` records the full source SHA and the encoded ID. The host displays the ID in its connection toast and decodes it as before. The HELLO transcript uses a `{build}` placeholder so both the default-zero host C runner and a custom nonzero-ID C build exercise the exact INFO frame. The actual SDK build still needs CI after the commit is pushed; historical hardware traces with `build_id=0` remain unidentifiable.

## 8. Suggested order (one behavior per commit, firmware changes validated by CI only)
1. Host: BUSY and credit handling, no `drop()` for protocol conditions, reserved control credit, raw lines in the UI log (H1-H5).
2. Firmware: link policy replaces the 5 s watchdog (F3), COILS (F4), control plane always answers (F1, F2) with tests; spec and matrix.
3. Confirm the 2x tick (read the SDK, bench time one long MOVE), then fix the timer or the labels, reported period and timeouts.
4. Calibrate half-steps per mm with a ruler; fix `STEPS_PER_MM` if needed.
5. Boot wiggle (W1-W4) with tests.
6. Temperature in C on the host.
7. Heater: UI sends `slices = 4` with sensible burn and shows the real per-slice limit; bench diagnostics (section 6).
8. Executor stuck-detection (F5), RX resync and HELLO loss (F6, 1.4), build id.

## 9. Acceptance

### Step 1 implementation note (host robustness)

Host changes are in progress on `pinbridge`: STAT/ESTOP can use the reserved final window credit, keepalive no longer skips solely because jobs are pending, BUSY/QFULL are retried with the same command ID, protocol-level ERR/sequence/timeout conditions no longer call the port-drop path, and raw non-TBP UART lines are emitted to the WebUI event log. The retry/credit and raw-line behavior has fake-transport coverage. These are host-side tests only; no device, firmware, or hardware evidence is implied. The allowed Bun, Python, generated-file, C transcript, and diff checks pass. Recovery from an ambiguous concurrent sequence mismatch is not yet complete: the host keeps the UART open and reports the ambiguity, but does not yet perform an automatic in-place resynchronization.
### Step 3 implementation note (step timer)
### Step 3 implementation note (period, speed and timeouts)

The host now rounds requested periods up to the same 1 ms quantum as the firmware before encoding MOVE/ROW. This makes the payload, firmware `period_eff_us`, and host timeout estimates use the same duration; a requested 1042 us period is treated as 2000 us, not 1042 us. The WebUI displays the effective quantized speed beside both speed inputs, so a request that cannot be represented exactly does not masquerade as the actual speed. Bun unit tests cover quantization and conversions. The reported ~2x trace error still needs the owner stopwatch test after the timer change; no hardware confirmation is implied.


The trace shows a consistent ~2x duration error for periods 1000–4000 us. Jieli SDK-family timer documentation maps `sys_s_hi_timer` to low-power priority 0 and warns its period may stretch when low-power ticks are skipped; the adapter used that timer continuously. The adapter now registers the priority-1 `sys_hi_timer_add(..., 1)` only while a step train or boot wiggle is active and deletes it when idle, with safe cancellation if registration fails. A static host regression test protects this SDK-bound choice. This is a source-based fix, **not yet confirmed** against the exact BR23 SDK build or on hardware; owner stopwatch timing after all software tasks remains required. SDK reference: [Jieli system timer documentation](https://doc.zh-jieli.com/AC79/zh-cn/release_v1.1.0/module_example/system/system_timer.html), with family-specific semantics also described in [Jieli's timer note](https://gitee.com/Jieli-Tech/fw-AC63_BT_SDK/issues/I4O1CV).

### Step 2 / F1 implementation note (immediate controls)

The immediate-control paths are covered under queue pressure: `tbp-control-plane-full-queue.txt` fills all four credits, starts work, then successfully issues STAT, CONFIG, FLUSH and HELLO; the existing `tbp-estop-busy.txt` covers ESTOP at a full queue. STAT is also tested while a heat-bearing ROW is active (`tbp-control-plane-bound.txt`), while HOT_WAIT is active (`tbp-heat-gates.txt`), and during DUTY_WAIT (`tbp-stat-duty-wait.txt`), in addition to the existing DWELL-running STAT transcript. This confirms portable parser/executor behavior, not USB/RTOS response latency on the physical device.

### Step 2 / F2 implementation note (control-plane bound)

The protocol spec now states a 50 ms ACK/ERR response bound on a healthy link and prohibits immediate commands from waiting for queue capacity or queued work. `tbp-control-plane-bound.txt` exercises STAT while a heat-bearing ROW is in preparation and motion, and checks that each explicit executor poll consumes at most 50 ms of simulated delay. This is a portable C test of explicit waits and protocol handling, not a measurement of MCU instruction time, RTOS scheduling, USB latency, or hardware response.

### Step 2 / F4 implementation note (COILS while busy)

COILS received while queued/running work exists is now accepted as a no-op with DONE; it does not change motor outputs or return BUSY. When idle, the existing bounded-hold safety gate for COILS-on remains in force. `tbp-coils-busy-noop.txt` checks the ACK/DONE lifecycle, confirms no motor writes, and confirms the active DWELL still completes. This C transcript failed before the change (ERR BUSY) and passes after it.

### Step 2 / F3 implementation note (legacy watchdog)

The legacy 5-second output watchdog now runs only outside an active TBP session. An active TBP session is governed by the configured TBP link timeout (which remains opt-in); when configured, that timeout cancels work into a safe state while retaining the session and answering subsequent commands. The `tbp-no-legacy-watchdog.txt` C transcript advances a long-running DWELL beyond five seconds and then confirms STAT still reports the running command. Existing `tbp-link-timeout.txt` continues to cover configured timeout cancellation and session retention. This is portable C simulation/transcript evidence only; no firmware SDK build, CI, flash, or hardware test was performed.

- The host never closes the port in the fake-transport fault tests; reconnect only on transport loss.
- STAT answers within the bound in every executor stage and with a full window; no raw `!wdg`.
- A timed bench MOVE matches its reported effective period; UI mm/s is within 5% of a stopwatch measurement.
- Boot wiggle moves 5 mm out and back with a pause and does nothing without paper (bench).
- UI shows C for the head temperature.
- Heater: either a visible mark on thermal paper or a documented measurement of why not (bench, owner-run).
- Local checks pass (`bun run check`, Python unittests, `./scripts/check-pinbridge.sh`, `git diff --check`, C transcripts), CI is green on the pushed commit.
  Hardware results are reported separately from CI and simulation.
