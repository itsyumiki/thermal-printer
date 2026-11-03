# Thermal Printer Binary Protocol (TBP) — v1 draft

Status: **design draft v0.1, 2026-11-02; Sprint 2 executor work in progress. Firmware CI builds are not hardware verification.**
Values marked **TBD** are owner decisions or need measurement. Section 14 lists every unverified assumption.

### Current implementation boundary

The Bun host implements TBP framing/CRC, sequential IDs, bounded retry recovery, a credit-gated four-command outstanding window, and ROW/MOVE/DWELL payloads. The C core advertises `queue_depth=4` and `max_window=4`, has a fixed four-command FIFO, and advances ROW/MOVE/DWELL from the poll loop. QUEUED/RUNNING/PROGRESS/terminal states, FIFO cancellation, ESTOP queue clearing, link-timeout cancellation and watchdog cancellation have C transcript coverage. This is still **not full v1 conformance**: the full parser and executor share the command task, although a higher-priority USB RX task independently scans CRC-valid ESTOP frames, cuts PB11 low and requests an executor abort before parser-side cancellation. Heating is now fail-closed: INFO reports zero heater/duty/shift limits and no sensor capabilities; nonzero ROW burn, legacy PHEAT/heat-bearing PEVENT, and direct PB11-high requests are rejected. PC4/PC5/PB3 are exposed only as unvalidated raw inputs. Heartbeat and link timeout are configurable, while nonzero coil hold is rejected and motor outputs release after motion. No validated temperature/battery/paper sampling, calibrated heater timer, nonzero duty budget, or hardware measurement of cancellation/ESTOP latency is available. Host/C tests and remote builds do not establish hardware safety.

This replaces the G-code-style idea in `thermal-design-doc.md` (still untracked, not edited). The queue and look-ahead
goals of that document are kept; the text syntax, the `R`/`P` staging commands and printer-side calibration are dropped.

## 1. Goals and decisions

- **Binary fields, hex-encoded on the wire, newline-delimited.** Easy to resync, easy to log, no tokenizing pitfalls.
- **Host-generated command ids; the printer echoes them.** Every command gets an immediate ACK and later STATUS frames.
- **Delayed work is queued, immediate work is not.** The parser never blocks on a full queue, so emergency stop always works.
- **Every row is self-contained.** A row carries its own mask, burn time, motion and period. There is no hidden state to desync.
- **Continuous motion.** The stepper runs nonstop on a timer; the heater runs on a separate timer interrupt; the next row is shifted while the current one burns. Consecutive motion commands never stop the motor or release the coils.
- **Gray is repeated 1-bit passes** planned by the host (section 11.5). The head has one heater strobe and one bit per dot.
- **Compensation and calibration live on the host.** The printer is stateless: no flash profile. It only enforces hard compiled-in safety limits (section 9).
- **Queue-aware host.** The host knows the printer's queue size and models command durations so the queue never runs dry or overflows.

Non-goals for v1: gray-depth rows, printer-side calibration, flash storage, acceleration ramps, power-off, LED and button control.

## 2. Conventions

- Multi-byte integers are **big-endian**. `i8`/`i32` are two's complement. `u8`/`u16`/`u32` are unsigned.
- "Hex" means ASCII hex digits, two per byte. The printer emits **uppercase**; receivers accept either case.
- Time units are microseconds (`_us`) or milliseconds (`_ms`) as named. "Step" always means one **half-step** of the stock eight-state phase sequence on `(PA9, PA10, PC3, PC2)`.
- **Direction:** positive steps feed paper forward (the printing direction); negative retracts.
- Dots: 384. Mask is 48 bytes; bit 7 of byte 0 is the leftmost dot. Physical dot order is **unverified** (section 14).

## 3. Framing

One frame per line. Same layout in both directions.

```
<ID><OP><LEN><DATA...><CRC16>\n
```

| Field | Size | Meaning |
|---|---|---|
| ID | 1 byte | Command id. Host-generated; printer echoes it in every response. `00` is reserved (section 4). |
| OP | 1 byte | Host to printer: opcode (section 7). Printer to host: response kind (section 5). |
| LEN | 1 byte | Number of DATA bytes, 0 to 255. |
| DATA | LEN bytes | Opcode- or kind-specific payload. |
| CRC16 | 2 bytes | CRC-16/CCITT-FALSE over ID, OP, LEN and DATA (poly `0x1021`, init `0xFFFF`, no reflection, xorout `0`). |

- A frame is 5 + LEN bytes, so 10 + 2·LEN hex characters, then `\n` (`0x0A`). Maximum wire line length is 521 characters including `\n` (520 hex characters plus LF). Implementations must allocate an additional NUL byte if using C strings; this storage byte is not part of the wire limit.
- Receivers ignore a `\r` immediately before `\n` and ignore empty lines. An embedded `\r`, spaces, odd digit counts and non-hex characters are a format error. In the firmware receiver, a malformed line with at least four leading hex characters is treated as a TBP candidate and returns `ERR FORMAT`; word-led legacy commands (including `ADC`) remain on the legacy parser path.
- CRC check value: CRC of the ASCII string `123456789` is `0x29B1`.
- The CRC does not make anything safe. Safety is enforced by firmware limits regardless of frame validity (section 9).

Reference CRC (C):

```c
uint16_t tbp_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}
```

### 3.1 Receive checks, in order

The printer validates each line in this order and stops at the first failure.

| # | Check | On failure |
|---|---|---|
| 1 | Hex/format: valid hex, even length, at least 5 bytes, `LEN` matches the byte count | `ERR FORMAT` (ID = first byte if present, else `00`). Overlong lines are discarded to the next `\n`. |
| 2 | CRC | `ERR CRC` with the received ID as a **hint only**. No ACK. |
| 3 | Opcode `FF` | Execute emergency stop immediately (section 6.3). Skips every later check. |
| 4 | Opcode is `00` or unassigned | `ERR BADOP` |
| 5 | Opcode `01` (HELLO) | Handled per section 7.1. Skips the later session, stopped and sequence checks. |
| 6 | Session active | else `ERR NOSESSION` |
| 7 | Printer not STOPPED, except for `STAT` | else `ERR STOPPED` |
| 8 | Sequence (section 4.2) | `ERR SEQ` (arg = expected id), or duplicate handling |
| 9 | `LEN` valid for this opcode | `ERR LEN` |
| 10 | Field ranges | `ERR RANGE` (arg = byte offset in DATA of the first bad field) |
| 11 | Class rules: idle-only commands, queue space | `ERR BUSY` or `ERR QFULL` |
| 12 | Accept: expected id advances, command enters its class, ACK sent | |

A command rejected at checks 9 to 11 does **not** consume its id. Frames the host already sent after it will then get `ERR SEQ`, which stops the stream at the right place (see 4.3).

## 4. Ids, sequencing and retries

### 4.1 Id space

- Ids are `01` to `FF`, 255 values. After `FF` the next id is `01`; `00` is skipped.
- `00` is used only by the printer, for unsolicited EVENT frames (section 5.4). The host never sends `00`.
- Opcode `FF` (emergency stop) and id `FF` are unrelated.

### 4.2 Strict sequential acceptance

The printer keeps `next_id`. For an incoming id, let `d = (id - next_id) mod 255` over the 255-value id cycle:

| d | Meaning | Printer action |
|---|---|---|
| 0 | The expected id | Accept (continue checks 9 to 12) |
| 128 to 254 | Duplicate of one of the last 127 ids | Reply `ACK`, then replay the **recorded** latest STATUS for that id (state only, no payload). Never re-execute. |
| 1 to 127 | A future id (a frame was lost) | `ERR SEQ`, arg = `next_id` |

`HELLO` sets `next_id` to its own id plus 1. The host must never have more than **127** commands without a terminal status outstanding, so the three ranges can never be ambiguous. The printer's HELLO reply reports a lower `max_window` if its record memory is smaller.

The printer records one byte of terminal state per accepted id for the current HELLO session, retaining the most recent 127 IDs logically. A duplicate response is `ACK` plus `STATUS` containing only the recorded state byte (no result payload). IDs outside retained history are rejected with `ERR SEQ`, arg = `next_id`; they must never execute as duplicates. Since the current synchronous implementation accepts only after validation and records terminal `DONE`, the state-only replay is sufficient for its present command model. When asynchronous QUEUED/RUNNING commands are introduced, Sprint 2 must retain and replay the actual latest state rather than assuming every accepted command is already DONE.

### 4.3 Host retry rules

- If no ACK, ERR or STATUS arrives for a frame within the ack timeout (suggested 200 ms USB, 1000 ms BLE), resend the **identical frame with the same id**. Duplicates are safe.
- On `ERR SEQ` (arg = `e`): the rejected frame was not accepted. Resynchronize at `e` and retry the same command payload there; because the current printer advertises a one-command window, serialize host transactions and do not have later frames in flight. Then assign following commands from the updated expected ID.
- On `ERR CRC`: do not trust the id. The next in-sequence frame will produce `ERR SEQ`, or the ack timeout will fire; recover through those.
- On any other `ERR` (`LEN`, `RANGE`, `BUSY`, `QFULL`, `BADOP`): the command was not accepted and its id is still expected. The host must stop sending later frames, fix the cause and resend from that id. It must not reuse the id for a different command until the old one is accepted.
- Three consecutive ack timeouts: treat the link as down. Send `FF` repeatedly (it is idempotent) until it is acknowledged, then start a new session with `HELLO`.

## 5. Responses

All responses use the request's id and the same frame layout. Response kinds have the high bit set so a capture is unambiguous about direction.

| Kind | Name | Meaning |
|---|---|---|
| `81` | ACK | Frame accepted. Sent immediately, never waiting on the queue. |
| `82` | STATUS | Progress or result of an accepted command. Zero or more per command, ending in a terminal state. |
| `83` | ERR | Frame rejected. No ACK. The command was not accepted. |
| `84` | EVENT | Printer-originated, always id `00`. |

Order for an accepted command: `ACK`, then STATUS frames, ending in exactly one terminal state (`DONE`, `FAILED` or `CANCELLED`). For immediate commands the ACK is followed at once by the terminal STATUS carrying the result. Nothing follows a terminal state.

### 5.1 ACK (`81`)

`DATA = [queue_used u8]`: number of queued commands accepted and not yet terminal, including the running one, after this command was accepted.

### 5.2 STATUS (`82`)

`DATA = [state u8][body...]`

| State | Name | Body | When |
|---|---|---|---|
| `01` | QUEUED | `[pos u8][queue_used u8]` | First after the command has waited 1000 ms without starting, then every 1000 ms until it starts. `pos` 0 is next to run. |
| `02` | RUNNING | none | Execution started. Sent only if the command's nominal duration is at least 250 ms. |
| `03` | DONE | command-specific result | Terminal, success |
| `04` | FAILED | `[code u8][arg u8]` | Terminal, execution failed. Fail-stop applies (section 9.3). |
| `05` | CANCELLED | `[reason u8]` | Terminal, never ran or aborted: `01` FLUSH, `02` ESTOP, `03` FAIL_STOP (an earlier command failed), `04` LINK_TIMEOUT, `05` NEW_SESSION |
| `06` | PROGRESS | `[remaining u32]` | Every 1000 ms while a command that runs 1000 ms or longer is executing. Units: steps for ROW/MOVE, ms for DWELL. |

### 5.3 ERR (`83`)

`DATA = [code u8][arg u8]`. Codes are in section 10. For `CRC` and `FORMAT` the id is a hint and may be wrong; if no id could be read it is `00`.

### 5.4 EVENT (`84`, id `00`)

`DATA = [event u8][body...]`. Events are not acknowledged by the host.

| Event | Name | Body |
|---|---|---|
| `01` | HEARTBEAT | The 13-byte STAT body (section 7.3). Sent every `hb_interval_ms` (default 1000, session active only). |
| `02` | PAPER | `[paper u8]` paper state changed |
| `03` | HOT | `[active u8]` `1` = head over temperature limit, waiting; `0` = cleared |
| `04` | RESET | `[reason u8]` `01` power-on, `02` watchdog, `03` other. Sent at boot. The session is gone. |
| `05` | STOPPED | `[reason u8]` firmware-initiated stop (same effect as opcode `FF`) |

## 6. Execution model

### 6.1 Command classes

| Class | Behaviour |
|---|---|
| **Immediate** | Executed inside the RX handler when accepted. Terminal STATUS follows the ACK at once. Does not touch the queue. |
| **Queued** | Placed in a FIFO of depth `queue_depth` (reported by HELLO). Executed in order by the executor. |
| **Emergency** | Opcode `FF` only. Bypasses sequencing, session and state checks. |

Every command except `HELLO` and `FF` consumes an id in sequence, whatever its class.

### 6.1.1 Control-plane response bound

On a healthy link, a well-formed frame must receive its ACK or ERR within **50 ms** of being made available to the firmware RX path. Immediate commands (HELLO, STAT, FLUSH, CONFIG, COILS, and ESTOP) must not wait for queue capacity or for a queued command to finish. The executor must return to RX dispatch between bounded work slices; no individual executor poll may perform an unbounded wait. The portable C test bounds explicit simulated delays per poll and exercises STAT during ROW preparation and motion. This does not measure MCU instruction time, RTOS scheduling, or USB transport latency; those remain hardware/CI validation items.

### 6.2 Queue and credits

- `queue_used` counts accepted queued commands that are not terminal, including the one running.
- The host must never send a queued command when its own count of unfinished queued commands equals `queue_depth` (its **credits** are zero). The printer replies `ERR QFULL` if this is violated; it should not happen in normal operation.
- Any `ACK` carries the current `queue_used`, so the host can re-sync its model at every frame.

### 6.3 Emergency stop (`FF`)

On receipt of a CRC-valid `FF` frame, regardless of queue state, session or id:

1. Heater off immediately; heater timer stopped.
2. Stepper timer stopped. In the current build `idle_hold_ms=0`, so coils release immediately after motion; nonzero hold is rejected until coil thermal limits are measured.
3. Queue discarded. State becomes **STOPPED**.
4. Reply `ACK`, then `STATUS DONE` with body `[03][flushed u8]`, then `STATUS CANCELLED` (reason `02`) for the running command and each queued command, in id order.

While STOPPED, only `HELLO`, `FF` and `STAT` are accepted; everything else gets `ERR STOPPED`. A `HELLO` ends the stop and starts a fresh session. `FF` is idempotent; the host should resend it until ACKed.

Implementation requirement: the RX path that recognises `FF` must run independently of the executor (interrupt or high-priority task) so it works while the queue is full and a row is burning.

### 6.4 Printer states

`01 IDLE` (queue empty, nothing running) · `02 BUSY` (queued or running work) · `03 STOPPED` (emergency stop) · `04 WAIT_HOT` (a row is waiting for the temperature gate) · `05 NO_SESSION` (no HELLO yet; shown only in boot-time reports).

## 7. Opcodes (host to printer)

| Opcode | Name | Class | Needs idle |
|---|---|---|---|
| `00` | **reserved, never valid** | | |
| `01` | HELLO | session | no |
| `02` | STAT | immediate | no |
| `03` | FLUSH | immediate | no |
| `04` | CONFIG | immediate | no |
| `10` | ROW | queued | no |
| `11` | MOVE | queued | no |
| `12` | DWELL | queued | no |
| `20` | COILS | immediate | **yes** |
| `21`–`EF` | unassigned (`ERR BADOP`), kept free for future use | | |
| `F0`–`FE` | unassigned, reserved for vendor/diagnostic use | | |
| `FF` | **EMERGENCY STOP** | emergency | no |

### 7.1 `01` HELLO

Starts or restarts a session.

- Request `DATA = [proto_ver u8]`. v1 is `01`. Unsupported version gives `ERR VERSION`, arg = highest supported version.
- Exempt from sequencing: `next_id` becomes `id + 1` (`01` after `FF`).
- Effects: heater off, stepper stopped, queue discarded (running and queued commands get `CANCELLED` reason `05`), STOPPED state cleared, state IDLE, CONFIG values reset to defaults.
- Reply: `ACK`, then `STATUS DONE` with the **INFO body**:

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | proto_ver | u8 | Protocol version implemented |
| 1 | build_id | u32 | First 8 hex digits of the source commit SHA, interpreted as a big-endian u32; zero means not injected/unknown |
| 5 | dots | u16 | 384 |
| 7 | queue_depth | u8 | Queued-command capacity |
| 8 | max_window | u8 | Max outstanding commands without a terminal status (at most 127) |
| 9 | min_period_us | u16 | Smallest accepted step period |
| 11 | step_tick_us | u16 | Step timer resolution; periods are rounded **up** to a multiple of this |
| 13 | burn_tick_us | u16 | Heater timer resolution; `0` means no validated heater timer |
| 15 | burn_max_us | u16 | Maximum `burn_us` per ROW; `0` disables all heating |
| 17 | guard_us | u16 | Minimum heater-off gap; `0` means unavailable/unvalidated |
| 19 | duty_window_ms | u16 | Duty-cycle window length; `0` means unavailable |
| 21 | duty_max_us | u16 | Maximum total heater-on time per window; `0` means unavailable |
| 23 | shift_us | u16 | Time to shift one 384-bit row; `0` means unmeasured |
| 25 | temp_limit_raw | u16 | Head temperature gate, raw ADC counts |
| 27 | batt_min_raw | u16 | Minimum battery, raw ADC counts |
| 29 | caps | u8 | bit 0 temp sensor valid, bit 1 battery sensor valid, bit 2 battery mV calibrated, bit 3 paper sensor valid |
| 30 | uid | 8 bytes | Chip unique id if available, else zeros. Hosts use it to select a calibration profile. |

INFO body is 38 bytes, excluding the leading `state` byte in the enclosing STATUS DATA. Thus the full STATUS DATA is 39 bytes. All limits are fixed in firmware and not writable by the host. `caps` is computed from the sensors' live validity at HELLO time; a capability bit is never set unless a real reading was just obtained. `temp_limit_raw` is the raw count at or below which the head is too hot to start heating (the NTC is the low side of the divider, so hotter means *lower* raw). `batt_min_raw` is `FFFF` because this firmware gates the battery in millivolts (see STAT `batt_mv`); hosts must accept caps bit 2 (battery mV calibrated) instead.

### 7.2 `03` FLUSH

Orderly stop. `DATA` empty.

Heater off at once; the running command is aborted at the next step boundary; all queued commands are discarded. The state returns to IDLE and the session continues. In the current build coils release immediately because `idle_hold_ms=0`. Reply: `ACK`, `STATUS DONE` body `[03][flushed u8]`, then `STATUS CANCELLED` (reason `01`) for each command, in id order. The printer does not need a HELLO afterwards.

### 7.3 `02` STAT

`DATA` empty. Reply: `ACK`, then `STATUS DONE` with the 13-byte **STAT body**, also used by HEARTBEAT:

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | state | u8 | Printer state (section 6.4) |
| 1 | queue_used | u8 | |
| 2 | queue_depth | u8 | |
| 3 | running_id | u8 | `00` if nothing is running |
| 4 | last_terminal_id | u8 | Most recent id that reached a terminal state |
| 5 | head_temp_raw | u16 | Raw ADC counts, `FFFF` if unavailable |
| 7 | batt_raw | u16 | Raw ADC counts, `FFFF` if unavailable |
| 9 | batt_mv | u16 | Millivolts, `FFFF` if not calibrated |
| 11 | paper | u8 | `00` absent, `01` present, `FF` unknown |
| 12 | flags | u8 | bit 0 temp gate active, bit 1 duty gate active, bit 2 coils energised, bit 3 heater on |

### 7.4 `04` CONFIG

`DATA = [key u8][value u16]`. Reply `STATUS DONE` body `[03][key u8][effective u16]`. Out-of-range values give `ERR RANGE`, never a silent clamp.

| Key | Name | Range | Default |
|---|---|---|---|
| `01` | hb_interval_ms | `0` (off) or 100 to 60000 | 0 (off) |
| `02` | idle_hold_ms | `0` (release immediately) or 100 to 10000 | 0; coils are released automatically when the hold expires |
| `03` | link_timeout_ms | `0` (off) or 500 to 60000 | 0 (off) |

CONFIG validates and stores effective values. Heartbeat interval and link timeout are applied; heartbeat is opt-in and disabled by default. Key 2 accepts only zero because coil thermal limits are unmeasured. Motor outputs remain high-impedance until motion is requested and release immediately after completion. With `link_timeout_ms` set, if no valid frame arrives for that long while work is queued or running, the printer behaves as FLUSH with `CANCELLED` reason `04`. Interface disconnect always has this effect immediately (section 12).

### 7.5 `10` ROW — print one row

Queued. `DATA` is exactly **54 bytes**; any other length gives `ERR LEN`.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | mask | 48 bytes | 384 dots, 1 = burn. Bit 7 of byte 0 is the leftmost dot. |
| 48 | burn_us | u16 | Total heater-on time for this row, `0` to `burn_max_us`. `0` = no heat. |
| 50 | steps | i8 | Half-steps during this row, -128 to 127. Sign = direction. `0` = stationary burn. |
| 51 | period_us | u16 | Time per half-step, at least `min_period_us`. Ignored (send `0`) when `steps = 0`. |
| 53 | slices | u8 | Number of equal heater pulses spread evenly across the row, 1 to `max(|steps|, 1)`. Must be 1 when `steps = 0`. |

A ROW frame is 59 bytes: 118 hex characters plus `\n`, 119 characters per row.

Validation (`ERR RANGE`): `burn_us > burn_max_us`; `steps != 0` and `period_us < min_period_us`; `slices = 0` or `slices > max(|steps|,1)`. A row with `burn_us = 0` or an all-zero mask performs no shift, latch or heat; it only steps.

`STATUS DONE` body (12 bytes):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | state | u8 | `03` |
| 1 | head_temp_raw | u16 | ADC sample taken when this row's heater was armed |
| 3 | batt_raw | u16 | same moment |
| 5 | batt_mv | u16 | same moment, `FFFF` if not calibrated |
| 7 | burn_eff_us | u16 | Actual total heater-on time after tick quantisation |
| 9 | period_eff_us | u16 | Actual step period used (`0` if `steps = 0`) |
| 11 | flags | u8 | bit 0 COLD_START (row started with the motor stopped), bit 1 STRETCHED (period increased to fit burn and guard), bit 2 LATE (boundary delayed because the next mask was not shifted in time), bit 3 DUTY_WAIT, bit 4 HOT_WAIT, bit 5 NO_HEAT |

The host should log these. `COLD_START` mid-image tells the host the queue underran.

### 7.6 `11` MOVE — feed or retract without heat

Queued. `DATA = [steps i32][period_us u16]` (6 bytes). `steps` is non-zero; `period_us >= min_period_us`. The heater is never enabled. Reply `STATUS DONE` body `[03][period_eff_us u16]`. Runs of 1000 ms or longer report PROGRESS every second. Cancelled by FLUSH or `FF` at the next step boundary.

### 7.7 `12` DWELL — wait with the heater off

Queued. `DATA = [ms u16]` (2 bytes), 1 to 65535. Coils stay energised and the heater stays off. Use it for head cooling between passes. Reply `STATUS DONE` body `[03]`.

### 7.8 `20` COILS

Immediate. `DATA = [mode u8]`: `00` release coils, `01` energise coils at the current phase. If queued/running work exists, accept the command as a no-op (do not change motor outputs) and reply `STATUS DONE` body `[03]`; never interrupt motion or return BUSY for this condition. When IDLE, mode `01` still requires a configured bounded coil hold (`CONFIG idle_hold_ms`), otherwise return the existing safety rejection. Mode `00` releases coils when IDLE.

### 7.9 `FF` EMERGENCY STOP

`DATA` empty. Behaviour in section 6.3. The id may be any value except `00`; it is echoed in the responses and does not advance `next_id`.

## 8. Row execution and timing

### 8.1 Two independent timers

- The **step timer** runs continuously through a row's `|steps|` ticks at `period_eff_us`. It never waits for the heater or the CPU.
- The **heater timer** is armed at the start of the row and turns PB11 on and off by interrupt for each slice. It never waits for the stepper.
- The motor phase index, direction and coil state live outside any command. When the next command is a ROW or MOVE, motion continues across the boundary with no stop, regardless of direction. Only the period changes, instantly (v1 has no acceleration; the host ramps periods across rows for start and stop of long moves).

### 8.2 Sequence per ROW

1. **Pre-shift (previous command):** while earlier motion ticks run, the CPU shifts this row's mask into the head's shift register with LATCH inactive (PB8 high). If the mask was not already shifted when the row's turn arrives, the executor shifts it on demand and the row starts late (flag LATE).
2. **t = 0:** pulse LATCH (PB8 low then high) to move the mask into the head's latch; arm the heater timer; start counting step ticks.
3. **Heat:** slice `k` (0-based) starts at `k · T_row / slices` after t = 0. Each slice is `burn_us / slices` wide, rounded to `burn_tick_us`, minimum one tick when `burn_us > 0`. `burn_eff_us` is the sum.
4. **During the row:** the CPU shifts the next queued ROW's mask, if one is queued. Shifting never touches the latch line.
5. **t = T_row:** the next command starts at once if queued. Otherwise heat is already off, the stepper stops with coils energised, and the printer goes IDLE.

`T_row = |steps| · period_eff_us` for `steps != 0`; for `steps = 0`, `T_row = burn_eff_us + guard_us`.

### 8.3 Constraints and what the firmware does

| Constraint | If violated |
|---|---|
| Each slice plus `guard_us` fits in its spacing: `burn_eff_us / slices + guard_us <= T_row / slices` | Firmware **stretches** the step period until it fits. Flag STRETCHED. Burn energy is never reduced. |
| The last slice ends at least `guard_us` before t = `T_row` | Same stretch |
| The next mask's shift (`shift_us`) completes before `T_row` ends | The boundary waits. Flag LATE. The host avoids this by choosing `|steps| · period_us >= shift_us`. |
| Duty budget (section 9) | Row start waits with heat off until the budget allows. Flag DUTY_WAIT. |
| Temperature gate (section 9) | Row start waits with heat off and motor held. Flag HOT_WAIT, state WAIT_HOT. |

A stationary burn (`steps = 0`) is legal and is the building block for gray (section 11.5).

## 9. Safety

### 9.1 Compiled-in limits (reported by HELLO, not writable)

| Limit | Behaviour |
|---|---|
| `burn_max_us` per ROW | 10000 us. Larger values are rejected at parse with ERR RANGE, offset 48. A single heater pulse (`ceil(burn_us / slices)`) may not exceed the per-slice maximum of 2500 us (also ERR RANGE offset 48). Values are owner decisions derived from stock evidence (best stock quality 2.22 ms per event, hard cutoff 6 ms), not bench measurements. |
| Duty: at most `duty_max_us` heater-on per `duty_window_ms` | 60000 us per 200 ms. A row that does not fit waits with heat off (DUTY_WAIT flag, QUEUED progress); a single row larger than the budget fails INTERNAL. Window accounting is a fixed (not sliding) window. |
| `guard_us` and `burn_tick_us` | 100 us gap between slices; 1 us tick (software-timed, calibrated delay loop). |
| Head temperature | Stock conversion of PC4. Heating may not *start* at or above 70 degC; a row already heating aborts at 75 degC (OVERTEMP, fail-stop, queue cancelled). A hot row waits with heat off until the head is below 50 degC (state WAIT_HOT, EVENT HOT, HOT_WAIT flag) and fails OVERTEMP after 60 s. |
| Battery | Quarter-VBAT SDK channel in mV; below 3400 mV a heat row fails LOWBAT. |
| Paper | PB3 low = present. Absent at start or before any slice: FAILED NOPAPER. |
| Sensor invalid | Open/shorted/unreadable temperature, unreadable battery or paper: FAILED SENSOR (arg 01/02/03), heater off, queued jobs CANCELLED reason `03`. Gates are re-run before every heater slice. |
| Heater on only inside a ROW slice or a gated PHEAT/PEVENT pulse | Heater is off in every other state, including STOPPED, IDLE, session loss and boot. Direct PB11-high through MOD/SET is blocked because it bypasses every gate. |

### 9.2 What the host can and cannot do

The host chooses `burn_us` and everything else per row, but cannot raise any limit above. A buggy or hostile host can produce an ugly print or delayed rows, not a sustained burn.

### 9.3 Fail-stop

If any queued command ends `FAILED`: heater off, remaining queued commands get `CANCELLED` reason `03`, state returns to IDLE and the session continues. The host decides whether to resume by re-sending the remaining rows.

## 10. Error codes

Shared by `ERR` (`[code][arg]`) and `STATUS FAILED` (`[code][arg]`).

| Code | Name | Arg | Used in |
|---|---|---|---|
| `01` | CRC | 0 | ERR (id is a hint) |
| `02` | FORMAT | 0 | ERR (id is a hint or `00`) |
| `03` | BADOP | the opcode | ERR |
| `04` | LEN | expected length | ERR |
| `05` | RANGE | byte offset in DATA of first bad field | ERR |
| `06` | SEQ | expected id | ERR |
| `07` | QFULL | queue_depth | ERR |
| `08` | STOPPED | 0 | ERR |
| `09` | BUSY | 0 | ERR |
| `0A` | NOSESSION | 0 | ERR |
| `0B` | VERSION | highest supported version | ERR |
| `10` | NOPAPER | 0 | FAILED |
| `11` | LOWBAT | 0 | FAILED |
| `12` | OVERTEMP | 0 | FAILED |
| `13` | SENSOR | which sensor (`01` temp, `02` battery, `03` paper) | FAILED |
| `1F` | INTERNAL | 0 | FAILED |

## 11. Host behaviour

### 11.1 Credits and time target

- Credits = `queue_depth` minus the host's count of unfinished queued commands. Never send at credits zero.
- Keep a **time-based** amount of work queued, not a fixed row count. Estimate each command's duration:
  - ROW with `steps != 0`: `max(|steps| · period_us, burn_us + slices · guard_us)`
  - ROW with `steps = 0`: `burn_us + guard_us`
  - MOVE: `|steps| · period_us`; DWELL: `ms`
- Target queued time `T_target = max(150 ms, 3 × longest row time, 2 × link round-trip p99, 2 × BLE connection interval × (slave latency + 1))`, never below two rows, never above what credits allow.
- Refill whenever `queued_time < T_target`. Re-sync the model from every ACK (`queue_used`) and every DONE.
- BLE: the measured link had slave latency 10 (see `uart-bridge-comparison.md`). The target must cover it or the queue underruns.

### 11.2 Session start

Open the interface, send `HELLO` with a fresh id, read INFO, store the limits, send `CONFIG` for heartbeat and idle hold if the defaults are not wanted, then start the job. Select the calibration profile by `uid`, or let the user choose one.

### 11.3 Retries and recovery

Follow section 4.3. After a failed or cancelled row, the host knows exactly which ids reached DONE and resumes from the first one that did not.

### 11.4 Compensation and calibration

All on the host:

- Burn time per row is chosen from the host's calibration profile for this printer (keyed by `uid`) and from the latest DONE measurements.
- The head's thermal lag means bulk temperature changes slowly relative to a 2 s queue; use the temperature and battery values in each DONE to update the model. The host also knows what energy it sent recently and can model local heat carryover.
- Battery sag shows up in `batt_raw`/`batt_mv` in each DONE.
- Steps per mm is a host calibration (feed a known step count, measure the paper). The protocol carries steps only.

### 11.5 Gray as passes

One row of grayscale is several 1-bit passes with different masks and burn times. Intermediate passes are stationary rows, the last one carries the motion:

```
ROW mask1 burn=A steps=0
ROW mask2 burn=B steps=0
ROW mask3 burn=C steps=0
ROW mask4 burn=D steps=k period=p slices=s
```

- The host plans passes and burn times so total heat per duty window stays within `duty_max_us / duty_window_ms`, or accepts DUTY_WAIT stalls.
- Binary-weighted burn times (1×, 2×, 4×, 8×) give 16 levels in 4 passes but only additive energies; following an arbitrary perceptual curve needs more passes or approximation. This is a host decision.
- Stationary passes stop the paper momentarily. To keep motion continuous, spread the row's motion across the passes (small non-zero `steps` in each) instead of using `steps = 0`.
- A second full pass in reverse brings in backlash and misregistration; interleaving is preferred.

### 11.6 Stuck-paper fix

The earlier demo sent shift, latch, heat and a finite step command separately for every row and waited for each, which stopped the motor between rows. In TBP the host keeps the queue filled, rows merge in the executor, and the stepper timer is never stopped between consecutive rows.

## 12. Link and session behaviour

- USB-CDC disconnect, or a BLE disconnect: heater off immediately, stepper stopped, queue discarded, session ended. In the current build coils release immediately (`idle_hold_ms=0`). After reconnecting the host must send `HELLO`.
- No heartbeat or other unsolicited traffic before HELLO except `RESET` and `PAPER`/`HOT` events.
- BLE: frames may be split across writes; reassemble by `\n`. A ROW frame is 119 characters, which fits one write at the measured MTU of 185.

## 13. Examples

All frames below were generated and CRC-checked with the reference algorithm. Ids are arbitrary.

HELLO, protocol 1, id `01`:
```
01010101E654
```

ROW, id `05`: mask `AA`×48, `burn_us = 1500` (`05DC`), `steps = +8` (`08`), `period_us = 6000` (`1770`), `slices = 4` (`04`):
```
051036AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA05DC08177004366D
```
Breakdown: `05` id, `10` ROW, `36` LEN (54), mask, `05DC 08 1770 04`, CRC `366D`.

MOVE, id `06`: +200 steps at 3000 µs, and the same move in reverse (-200):
```
061106000000C80BB8ECC7
061106FFFFFF380BB84A26
```

DWELL, id `07`: 500 ms:
```
07120201F472DD
```

FLUSH, id `08`, and emergency stop (id `09`):
```
080300306E
09FF0051F2
```

Printer responses to the ROW above:
```
0581010337BD                     ACK, queue_used = 3
058203010203D1AF                 STATUS QUEUED, pos 2, queue_used 3 (after 1 s waiting)
05820C0303100A201F0E05DC1770009E38   STATUS DONE: temp 0x0310, batt_raw 0x0A20, batt 7950 mV,
                                    burn_eff 1500 us, period_eff 6000 us, flags 00
```

Other responses:
```
0A830206058525                   ERR SEQ, expected id 05
058302010029EE                   ERR CRC, id 05 as a hint
06820205025D0E                   STATUS CANCELLED, reason 02 (emergency stop)
09820203029251                   STATUS DONE for FF, body [03][flushed = 2]
00840E01020320050403100A201F0E010CE9F1   EVENT HEARTBEAT: BUSY, 3/32 queued, running 05, last done 04,
                                          temp 0x0310, batt_raw 0x0A20, 7950 mV, paper present, coils + heater on
```

Typical job: `HELLO`, `CONFIG`, several `ROW`s sent until credits or `T_target` are met, then one new `ROW` per DONE, a final `MOVE` to feed paper out, wait for its DONE.

## 14. Unverified assumptions and open items

1. **Head latch register is separate from the shift register**, so shifting during a burn is safe. This is typical for thermal heads, but not confirmed for this one. The stock firmware shifts, pulses LATCH, then lets the heater ISR run on the latched data (`printhead-protocol-and-heating.md`). If the assumption is wrong, the fallback is to shift only after a row's burn ends, which lowers the maximum row rate.
2. **Timer resolution.** A 100 µs hardware timer for the stepper was tried (`0dc044c`) and reverted (`c89d965`); the commit does not say why. The step and heater timer resolutions in INFO must be found out before relying on them.
3. **Dot order and byte order** in the shift register (bit 7 of byte 0 = leftmost dot) is unverified.
4. **Shift time per row** is unmeasured; `shift_us` in INFO is the number to establish first. It sets the maximum row rate.
5. **Temperature and battery:** PC4/ADC10 as head thermistor and PC5 as battery divider remain unvalidated hypotheses; raw ADC counts are available but not assigned safety meaning or calibrated.
6. **Paper sensor** PB3 mapping is unvalidated; raw GPIO level is not advertised as paper presence.
7. **Safe numeric limits** (`burn_max_us`, duty, `temp_limit_raw`, `batt_min_raw`, `guard_us`) remain unvalidated. The firmware reports zero heater capability until they are measured and reviewed.
8. **Coil hold** thermal limits are unknown; current `idle_hold_ms=0` releases coils after motion and nonzero values are rejected.
9. **Slice count:** the stock firmware fires four pulses per row across four phase steps. Whether more or fewer slices improve print quality is untested.
10. **Queue depth** depends on free RAM. A ROW slot is 54 bytes; 32 slots is about 1.7 KB.
11. **Throughput:** hex doubles the bytes. At 6 ms per row, about 167 rows/s is about 20 kB/s upstream; fine for USB-CDC, unverified on BLE. If BLE is the bottleneck, a binary-framed variant with the same fields can be added.
12. **Sign encoding:** `steps` uses two's complement (-128 to 127), not sign-magnitude.
13. **Chip unique id** availability on this BR23-family chip is unknown.
14. **Not in v1:** power-off, LED, button events, gray-depth rows, acceleration, flash profiles.

## 15. Relationship to the rest of the project

- The Pinbridge diagnostic commands (`PHEAD`, `PLATCH`, `PHEAT`, `PSTEP`, WebSocket demos) are unchanged. TBP is intended as a separate application firmware that may reuse Pinbridge's pin tables and head/stepper drivers, but this document does not decide that structure.
- Safety limits stay in firmware here, unlike Pinbridge's diagnostic path where the host is the policy layer.
- This design implements the direction in `stock-print-scheduler-handoff.md`: a firmware-side scheduler with continuous motion across rows, status and cancel. Items from its "known unknowns" that remain open are listed in section 14.
