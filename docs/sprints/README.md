# Pinbridge timing and executor rework — sprints

Status: active, 2026-11-05. The timing rework supersedes the earlier TBP protocol sprint plan. Sprint 0 has a documented static/derived analysis and scope plan, but no scope capture. Sprint 1 is closed: binary TBP `TIMETEST` passes on hardware for the four TIMER3 sources, CI is green on `a3270e1`, and the STEP, JIFFY, DELAY and DELAY2 sources have a recorded baseline (2026-11-05, build `a3270e1c`); the web UI has the timer self-test buttons again. Sprint 2 is code complete (timebase, `micros()`, timebase-based `hal_delay_us`, `TIMEBASE` self-test) and waits for CI and the owner's hardware run. The earlier protocol sprint plan remains in git history (`git show 9e22ffc:docs/sprints/README.md`).

## Why this exists

The 2026-11-05 `PTIME` self-test measured the firmware's clocks against the host's wall clock. Full background: `docs/timer-architecture-and-usage.md` and `docs/bugs/2026-11-04-heater-blank-prints-timing.md`.

| Clock | Assumed | Measured |
|---|---|---|
| Jiffies (`hal_ms`) | 10 ms | 10.00 ms |
| Heater TIMER3 oscillator (all four prescalers) | 24 MHz | 23.97-24.00 MHz (5 runs each, 2026-11-05, build `a3270e1c`) |
| TIMER3 compare IRQ | not known to fire | fires every period |
| TIMER3 `/4 /16 /64` rows | 24 MHz | **old table invalid** (the test counted each compare twice). Corrected runs on hardware (5 each): ratios 1.0014 / 1.0011 / 1.0007, `isr == n` |
| SDK "1 ms" step callback | 1 ms | **2.000 ms** (jiffy cross-check agrees; stock TIMER1 is fixed at 2.000 ms) |
| `hal_delay_us(1000)` | 1000 us | 940 us on build `a3270e1c` (881 us on the older PTIME build, same source loop) |
| `hal_delay_us(2)` | 2 us | **5.22 us** on build `a3270e1c` (about 3.5 us fixed + 0.85 us per loop iteration; 4.97 us on the older build). Bimodal between runs (2.51 s vs 2.67 s per 500,000 calls), cause unknown |

Code reading on the same day found that the TBP executor advances one stage per `pb_poll`, and `pb_poll` runs once per wake of `command_task` (host data or the 10 ms semaphore timeout). Neither the heater ISR nor the step callback wakes it, so stage handoffs can each wait up to 10 ms.

Heater pulse widths therefore look trustworthy, and the blank prints are probably not a TIMER3 problem. The stock firmware's pulse (about 0.72-2.22 ms, hard cap 6 ms if its 30 us tick is right) is still unexplained next to our need for 50 ms or more. These sprints fix the timing foundation and chase that question in parallel.

## Target architecture (to be confirmed in Sprints 0-2)

One cooperative loop owns all printer timing. TIMER3 becomes a free-running timebase, with at most one slow wrap interrupt that doubles as the heater deadman. Heater pulses, step phases, shifting and sensor reads are all deadline-driven from that timebase in `command_task`. The remaining interrupts are the SDK's own (USB, jiffies). The stepper no longer uses `sys_hi_timer`, which deletes the `step_lock` race workaround.

## Current verification snapshot (2026-11-05)

- Current branch: `pinbridge`, head `a3270e1`. CI (GitHub Actions) succeeded on `a3270e1` and on `ac07f95`. The workflow builds firmware; it does not run the Bun suite.
- Local tests: `bun test` passed 90 tests on the last run (after the web UI timer panel was added). The C harness is the other suite of record. The Python suite under `host/pinbridge` is dropped (see constraints below) and was not run for this snapshot. No local firmware build was attempted.
- Hardware (owner, 2026-11-05, build `a3270e1c`): all eight `TIMETEST` sources were run five times each from the web UI. The four TIMER3 rows pass the 2 % gate with `isr == n`. The STEP, JIFFY, DELAY and DELAY2 rows are baselines, not gated; the table is in `docs/timer-architecture-and-usage.md` section 3 (full baseline).
- No printer hardware was flashed or measured by the agent. There is still no PB11 or motor-phase scope capture.

## Sprint sequence

| Sprint | Goal | Exit gate | Handoff |
|---|---|---|---|
| 0 | Pin down the stock heater and step timing | Static/derived timing table and scope plan documented; actual stock PB11 scope capture still pending | [Sprint 0](sprint-00-stock-timing.md) |
| 1 | Make the timer self-test trustworthy | Binary `TIMETEST` passes CI and all four TIMER3 rows are within 2 % on hardware with `isr == n`. **Met 2026-11-05** (CI green on `a3270e1`; baseline for the other four sources recorded) | [Sprint 1](sprint-01-ptime-fix.md) |
| 2 | Free-running timebase and accurate delays | `micros()` HAL call, `hal_delay_us` within 2 %, deadman design proven. **Code complete 2026-11-05; hardware gate pending** | [Sprint 2](sprint-02-timebase.md) |
| 3 | Heater pulse in the executor, on the timebase | Scope-measured PB11 width within 5 % of request, deadman verified | [Sprint 3](sprint-03-heater-in-loop.md) |
| 4 | Main loop that polls while busy | Row throughput and e-stop latency measured, no TX starvation, no watchdog resets | [Sprint 4](sprint-04-main-loop.md) |
| 5 | Stepper in the loop, SDK step timer removed | `sys_hi_timer`, `step_lock`, race hooks gone; step period measured | [Sprint 5](sprint-05-stepper-in-loop.md) |
| 6 | Faster, bounded head shift | Shift time measured, nothing slow while PB11 is high | [Sprint 6](sprint-06-shift.md) |
| 7 | Validation and go/no-go | Bench checklist, pulse-width answer, documented decision | [Sprint 7](sprint-07-validation.md) |

## Dependency rule

Sprint 0 is read-only and can run alongside everything else. Sprints 1-2 come first, because every later sprint's measurements use them. Sprint 3 needs Sprint 2. Sprint 4 can start after Sprint 3, and Sprint 5 needs Sprint 4. Sprint 6 is independent after Sprint 2 but should not land before Sprint 4. Sprint 7 follows all of them.

## Project-wide constraints

- Label evidence as mock, transcript, CI build or hardware, and never claim hardware behavior from a compile or mock test.
- No flashing and no hardware writes without explicit owner approval. The owner runs the self-test and the scope captures.
- CI on `pinbridge` is the validation of record for firmware. A local scratch-harness build is only a compile check: use a throwaway tag, never overwrite `~/public/pinbridge-app.bin`, and do not report it as a CI result.
- Never run `cargo test` (see `~/AGENTS.md`), and never print secrets.
- Heater and stepper behavior may change freely during this rework (owner decision 2026-11-05). Sprint 2 may repurpose TIMER3 and break or replace `heater_pulse_*`; most of the heater and stepper timing is being reimplemented anyway. Safety gates and the firmware-enforced limits below still hold.
- The Python host suite (`host/pinbridge`, `unittest`) is dropped from the sprint gates (owner decision 2026-11-05: too much overhead for the current repo size). Do not run it, extend it or cite it as evidence. The tests are still in the tree; deleting them is a separate, explicit step.
- Firmware limits stay firmware-enforced. Host/UI limits are convenience only.
- Do not raise heater limits on mock or demo evidence. `PB_TUNE_HEATER_SLICE_MAX_US` and `PB_TUNE_HEATER_BURN_MAX_US` are test-only values from 2026-11-04 and must go back to stock-derived limits once Sprint 7 finishes.
- Keep each change to one behavior with its tests and docs. No opportunistic refactors.

## Definition of done for every sprint

1. Acceptance criteria are met, or marked blocked with evidence.
2. The C harness and the Bun suite pass, plus any contract tests the sprint touches. (The Python suite is dropped.)
3. `git diff --check` passes and the diff holds no secrets or unrelated edits.
4. The handoff records commands run and results, what was not tested, the commit SHA and the next sprint's starting point.
5. The commit is pushed to `pinbridge` and the CI result is known before success is reported.
