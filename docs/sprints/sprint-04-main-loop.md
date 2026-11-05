# Sprint 4 — Main loop that polls while busy

**Size:** M  
**Depends on:** Sprint 3  
**Goal:** Stop the executor from advancing once per 10 ms wake, without starving TX or the watchdog.

## Read first

- `command_task`, `tx_task`, `rx_task`, task priorities (rx 3, command 2, tx 1) in `app.c`
- `pb_poll` and `pb_tbp_exec_poll` in `pb_core.h`
- The 2026-11-05 `PTIME` finding that a busy command task starves TX

## Tasks

1. Measure first: row-to-row time and slice-to-slice time today, with a fixed test print, so the improvement is a number.
2. While anything is active (step, TBP command, heater, wiggle), loop on `pb_poll` without waiting on `cmd_sem`; sleep on the semaphore only when idle.
3. Raise TX above the command task, or send critical replies from the loop, and bound how long the loop runs between USB checks.
4. Call `wdt_clear` explicitly in the busy loop; document the longest busy stretch.
5. Re-check the executor stall watchdog (`PB_TUNE_EXECUTOR_STALL_TIMEOUT_MS`) against the faster cadence.

## Acceptance criteria

- Measured row time drops to the sum of the real shift, latch and heat times plus a documented overhead.
- ESTOP latency during a print is measured and not worse than today.
- A multi-minute print shows no watchdog reset, no dropped acks, and no TX starvation (reply latency measured).
- All suites pass; CI builds.

## Out of scope

Moving the stepper, speeding up the shift.

## Handoff on completion

Include before/after timings and the chosen priority scheme. Next: Sprint 5.
