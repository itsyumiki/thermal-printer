# Sprint 1 — Trustworthy timer self-test

**Status (2026-11-05): hardware acceptance met.** The TIMER3 loop counts ISR deltas only, and the Bun `TIMETEST` encoder/parser/summary plus firmware adapter are in the tree. The owner flashed build `ac07f950` and ran the four TIMER3 cases at 1,000 ms: every ratio is within 0.25 % of 1.0, the implied oscillator is 23.94-23.98 MHz, and `isr == n` (100 / 25 / 10 / 2). Full table: `docs/timer-architecture-and-usage.md` section 3. The web UI has a timer self-test panel again (binary `TIMETEST`, five runs per source). The remaining sources (STEP, JIFFY, DELAY, DELAY2) have not been run on this firmware, and CI for the commit that adds the UI panel has not been checked.

**Size:** S  
**Depends on:** nothing  
**Goal:** Fix the TIMER3 double count and preserve the self-test as a binary TBP `TIMETEST` diagnostic after removal of the legacy plaintext `PTIME` command.

## Read first

- `hal_time_test` in `firmware/apps/pinbridge/src/app.c`
- `TIMETEST` command/result parser in `host/pinbridge/bun/tbp/commands.ts`, `messages.ts` and `timetest.ts`
- `docs/timer-architecture-and-usage.md` section on TIMER3

## Tasks

1. In the TIMER3 loop, count a compare once. Either count only through `heater_isr_count` or only through the pending flag, and make the ISR-fired case unambiguous. Keep the `isr=` field.
2. Decide whether the test should leave the compare IRQ enabled. If so, record that restarting the timer per ISR adds a small bias and report it. Otherwise mask it for the run and report that separately.
3. Update the host summary so `impliedOscMHz` uses the corrected count, and keep the jiffy cross-check.
4. Keep a static contract check that the TIMER3 loop cannot count one compare through both the ISR counter and the pending flag, and keep the Bun tests for the host summary and binary command framing.
5. Correct `docs/timer-architecture-and-usage.md` and `docs/thermal-protocol-spec.md`: explain the old test bug, document binary opcode `F0` `TIMETEST`, and distinguish code/test evidence from hardware results.

## Acceptance criteria

- On hardware, all four TIMER3 rows report a ratio within 2 % and an implied oscillator near 24 MHz. **Met 2026-11-05.**
- `isr` per run matches the expected period count. **Met 2026-11-05.**
- Python/C and Bun suites pass; CI builds. Bun passes locally (90 tests); the firmware build with this `TIMETEST` code was flashed and ran. Python was not re-run for the UI change.

## Out of scope

New timer sources, any change to heater behavior.

## Current handoff

- The firmware counts `heater_isr_count` deltas only; it does not also count TIMER3 pending-flag observations. The IRQ remains enabled and the test restarts the timer after each ISR, so a small positive timing bias is possible and has not been measured.
- The legacy text `PTIME` path was removed with the legacy protocol. This change adds diagnostic TBP opcode `F0` (`TIMETEST`) with sources `T3D4`, `T3D16`, `T3D64`, `T3D256`, `STEP`, `JIFFY`, `DELAY`, and `DELAY2`; the host times the RUNNING-to-DONE interval and reports ratio, implied oscillator, and `isr == n`.
- Local checks pass: Bun 86/86 and Python suite 52/52. This is local test evidence only. The most recent CI success predates this change, so CI validation is still required.
- Hardware table (owner run, build `ac07f950`, 1,000 ms, one run each): /4 ratio 1.0025 (23.94 MHz), /16 1.0010 (23.98 MHz), /64 1.0010 (23.98 MHz), /256 1.0009 (23.98 MHz); `isr == n` in all four. The /4 row reads 0.25 % long, about 25 us of ISR-entry latency per restart; the others are within the 1 ms resolution of the host timing. No flashing was performed by this agent.
- Web UI: "Timer self-test" panel with one button per source plus "Run all" (about 50 s). The host action `time-test` runs `TIMETEST` N times, pauses the keepalive meanwhile, and refuses other commands until it finishes.
- Console gotcha: pasting `TIMETEST` frames by hand can collide with the host's 1.5 s STAT keepalive ids and come back as `ACK` plus an empty `DONE` without running. Not fixed; see `docs/timer-architecture-and-usage.md` section 3.

Next: run the STEP, JIFFY, DELAY and DELAY2 sources from the web UI and record them as the Sprint 7 baseline, then close Sprint 1 and start Sprint 2.
