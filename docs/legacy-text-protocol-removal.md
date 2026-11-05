# Removed Pinbridge plaintext protocol

The firmware no longer accepts the previous line-oriented ASCII command protocol. It has no fallback parser: every non-empty serial line must be a TBP v1 hexadecimal frame, and replies/events are hexadecimal TBP frames only. A plaintext line receives a TBP `ERR` with FORMAT code (`02`), not a text error.

The removed command families were:

- Session/debug/watchdog/reset: `HEL`, `DBG`, `WDG`, `RST`.
- GPIO and pin inspection: `MOD`, `SET`, `GET`, `STA`, `ADC`, `LSN`. These exposed pin-mode changes, writes, reads, ADC queries, and asynchronous pin listeners.
- Experimental printer primitives: `PHEAD` (manual 384-dot head shift), `PLATCH` (manual latch pulse), `PHEAT` (one gated heater pulse), `PSTEP`/`PSTEPSTAT` (standalone stepper move/status), and `PEVENT` (combined head/latch/heat/motion sequence).
- Diagnostics: `PDIAG`, `PHDIAG`, and `PTIME` (printer/heater/stepper diagnostics and blocking timer timing test).
- The old text event/watchdog replies such as `!<PIN>=<level>`, `!step done`, and `!wdg`, along with `ok ...` / `err ...` replies.

## TBP replacements

- `HEL` becomes the binary `HELLO` opcode (`01`), which establishes a session and returns INFO.
- Printer status and sensor data use `STAT` (`02`) and its defined status/event payloads.
- Printing is expressed through queued `ROW` (`10`), `MOVE` (`11`), and `DWELL` (`12`) commands, with `FLUSH` (`03`) and `ESTOP` (`FF`) for cancellation and emergency stop. `CONFIG` (`04`) controls supported session settings and `COILS` (`20`) requests the constrained coil behavior.
- Arbitrary GPIO control, text listeners, and unbounded low-level printer diagnostics are intentionally not part of the TBP v1 interface.

These commands are removed from firmware dispatch, not merely disabled at runtime. The old `pb_commands.h` module and plaintext printer-command parser have been deleted. Shared low-level motor, head, sensor and heater-safety helpers remain only where used by the TBP executor or local safety behavior.


The older `host/pinbridge/pinbridge/` Python CLI/WebUI and its text-command examples are not compatible with this firmware. The supported runtime is `host/pinbridge/bun/`, whose serial transport accepts and emits TBP hex frames only.
