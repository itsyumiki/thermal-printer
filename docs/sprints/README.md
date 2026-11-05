# Pinbridge timing and executor rework — sprints

Status: active, 2026-11-05. The timing rework supersedes the earlier TBP protocol sprint plan. Sprint 0 has a documented static/derived analysis and scope plan, but no scope capture. Sprint 1 has a binary TBP `TIMETEST` implementation, passing local host tests and a passing owner hardware run of the four TIMER3 sources (2026-11-05, build `ac07f950`); the web UI has the timer self-test buttons again. The earlier protocol sprint plan remains in git history (`git show 9e22ffc:docs/sprints/README.md`).

## Why this exists

The 2026-11-05 `PTIME` self-test measured the firmware's clocks against the host's wall clock. Full background: `docs/timer-architecture-and-usage.md` and `docs/bugs/2026-11-04-heater-blank-prints-timing.md`.

| Clock | Assumed | Measured |
|---|---|---|
| Jiffies (`hal_ms`) | 10 ms | 10.00 ms |
| Heater TIMER3 oscillator (`/256`, 2 periods, no double count) | 24 MHz | 23.98 MHz (re-measured 2026-11-05 on the corrected test: 23.98 MHz) |
| TIMER3 compare IRQ | not known to fire | fires every period |
| TIMER3 `/4 /16 /64` rows | 24 MHz | **old table invalid** (the test counted each compare twice). Corrected run on hardware: 23.94 / 23.98 / 23.98 MHz, ratios 1.0025 / 1.0010 / 1.0010, `isr == n` |
| SDK "1 ms" step callback | 1 ms | **2.000 ms** |
| `hal_delay_us(1000)` | 1000 us | 881 us |
| `hal_delay_us(2)` | 2 us | **4.97 us** (about 3.2 us fixed + 0.88 x requested) |

Code reading on the same day found that the TBP executor advances one stage per `pb_poll`, and `pb_poll` runs once per wake of `command_task` (host data or the 10 ms semaphore timeout). Neither the heater ISR nor the step callback wakes it, so stage handoffs can each wait up to 10 ms.

Heater pulse widths therefore look trustworthy, and the blank prints are probably not a TIMER3 problem. The stock firmware's pulse (about 0.72-2.22 ms, hard cap 6 ms if its 30 us tick is right) is still unexplained next to our need for 50 ms or more. These sprints fix the timing foundation and chase that question in parallel.

## Target architecture (to be confirmed in Sprints 0-2)

One cooperative loop owns all printer timing. TIMER3 becomes a free-running timebase, with at most one slow wrap interrupt that doubles as the heater deadman. Heater pulses, step phases, shifting and sensor reads are all deadline-driven from that timebase in `command_task`. The remaining interrupts are the SDK's own (USB, jiffies). The stepper no longer uses `sys_hi_timer`, which deletes the `step_lock` race workaround.

## Current verification snapshot (2026-11-05)

- Current branch: `pinbridge`. This change includes the timing implementation and host parser described here; the commit is awaiting CI validation and hardware acceptance.
- Local tests on the current working tree: `bun test` — 90 passed (after adding the web UI timer panel); `uv run --offline python -m unittest discover -s tests` from `host/pinbridge` — 52 passed; `git diff --check` — passed. No local firmware build was attempted.
- The last observed GitHub Actions success was for the preceding commit `6125d2c`, before these timing changes. Do not treat it as CI validation of `TIMETEST`.
- Hardware (owner, 2026-11-05 about 11:30, build `ac07f950`): the four TIMER3 `TIMETEST` rows pass the 2 % gate with `isr == n`; see `docs/timer-architecture-and-usage.md` section 3. No printer hardware was flashed or measured by the agent. The STEP, JIFFY, DELAY and DELAY2 rows have not been re-run.

## Sprint sequence

| Sprint | Goal | Exit gate | Handoff |
|---|---|---|---|
| 0 | Pin down the stock heater and step timing | Static/derived timing table and scope plan documented; actual stock PB11 scope capture still pending | [Sprint 0](sprint-00-stock-timing.md) |
| 1 | Make the timer self-test trustworthy | Current binary `TIMETEST` code passes CI, then all four TIMER3 rows within 2 % on hardware with `isr == n` (hardware rows met 2026-11-05; CI for the UI-panel commit not yet checked) | [Sprint 1](sprint-01-ptime-fix.md) |
| 2 | Free-running timebase and accurate delays | `micros()` HAL call, `hal_delay_us` within 2 %, deadman design proven | [Sprint 2](sprint-02-timebase.md) |
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
- Firmware limits stay firmware-enforced. Host/UI limits are convenience only.
- Do not raise heater limits on mock or demo evidence. `PB_TUNE_HEATER_SLICE_MAX_US` and `PB_TUNE_HEATER_BURN_MAX_US` are test-only values from 2026-11-04 and must go back to stock-derived limits once Sprint 7 finishes.
- Keep each change to one behavior with its tests and docs. No opportunistic refactors.

## Definition of done for every sprint

1. Acceptance criteria are met, or marked blocked with evidence.
2. Python/C suite and Bun suite pass, plus any contract tests the sprint touches.
3. `git diff --check` passes and the diff holds no secrets or unrelated edits.
4. The handoff records commands run and results, what was not tested, the commit SHA and the next sprint's starting point.
5. The commit is pushed to `pinbridge` and the CI result is known before success is reported.
