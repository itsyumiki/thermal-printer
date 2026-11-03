# Sprint 4 handoff — scheduler checkpoint

Status: **partial checkpoint; Sprint 4 acceptance is not complete.** This is software-only work. No local firmware build, flash, or hardware validation was performed.

## Commits

- `2c0a86b25e18094480d6d5ae84196ceca75476d4` — design decisions and truthful `COLD_START` result flag; transcript expectations updated.
- `20a6e9cfbcd46089d6c7a6cbe45bde91f7273029` — Bun `print-job` endpoint and demo submits one multi-row request, consumes row progress and terminal completion.
- Handoff/documentation commit: pending.

## Checks

Passed locally on the checkpoint:

- `cd host/pinbridge/bun && bun run check` — Prettier and Bun tests: 22 passed, 79 assertions.
- `./scripts/check-pinbridge.sh` — generated files OK.
- `git diff --check` — passed.
- `host/pinbridge/.venv/bin/python -m unittest discover -s host/pinbridge/tests -v` — 35 tests passed.
- `python3 -m py_compile host/pinbridge/examples/print_matrix_square_ws_demo.py` — passed.
- GitHub Actions run/artifact: pending push and remote workflow completion.

These checks establish software/mock behavior only. They do not establish physical feed direction, dot polarity, thermal safety, actual shift time, or continuous motion.

## Stock behavior: confirmed vs inferred

- **Confirmed by static stock-firmware analysis, not hardware measurement:** nominal 2 ms callback; divider at least 3 implies nominal 6 ms event-post interval before queueing; a stock logical row is processed as four same-mask shift/latch/heat events and four stepper phase advances; stock shift order is byte 0..47 and bit 0..7 (LSB first); the eight-state half-step sequence and head/stepper pin roles are described in the timing reference.
- **Confirmed in Pinbridge source/spec:** PB10 DATA, PB9 CLK, PB8 active-low latch, PB11 heater strobe; TBP bit 7 of byte 0 is the logical leftmost dot; positive TBP steps mean forward feed; phase index is persistent across commands; heat still goes through Sprint 3 gates.
- **Inferred/unverified:** the shift register is isolated from the currently latched head image; actual dot-to-wire mapping and black polarity; physical forward direction; exact head-driver electrical timings; stock ~30 us heater tick derived from timer setup; 3 ms shift estimate. No scope or paper-motion measurements exist.
- **Decision:** do not pre-shift the next row until register isolation is proved. `PB_TUNE_HEAD_SHIFT_TIME_US=3000` remains a planning estimate only. Timer-based motor service remains millisecond-granularity in the current portable executor; no sub-ms precision is claimed.

## What this checkpoint changes

- ROW DONE can report `COLD_START` when a moving row begins with no active step sequence. `STRETCHED` and `LATE` remain clear because the executor does not yet implement reliable period stretching or a row-deadline detector.
- The Bun host accepts one `print-job` request (up to 2000 ROWs), pipelines within the advertised queue depth, emits progress after terminal ROW statuses, and returns a final count. The demo builds one job with four identical TBP ROW events per logical image row and retains dry-run mode.
- Firmware still executes each ROW's shift/latch/heat sequence before starting that ROW's movement. Thus the queue avoids a host request per event, and phase/coil state is retained while queued work remains, but **the motor still stops at ROW boundaries**. This fails the Sprint 4 continuous-feed acceptance criterion. The added `COLD_START` flag makes that limitation visible rather than hiding it.

## Throughput limits

No measured throughput is available. A serialized row pays shift + latch + heat work plus its motor interval; pre-shift is intentionally disabled. The stock 6 ms event cadence and 3 ms shift time are static estimates, not Pinbridge timing guarantees. The demo's four-event representation does not prove stock-equivalent dot placement or thermal behavior.

## Required work before Sprint 4 can be called complete

1. Refactor the firmware executor into a fake-clock-testable scheduler that can overlap row preparation and motion and hand off queued ROW/MOVE work without stopping the phase timer at row boundaries. Do not pre-shift unless hardware analysis proves it safe.
2. Add deterministic scheduler-level tests for four-event accounting, phase sequence/direction, queue handoff, late shift, no-heat rows, burn slices, empty queue, and cancel/ESTOP at boundaries. Existing C transcripts cover protocol, gate, queue and cancellation behavior, but are not a complete scheduler model.
3. Detect and test `STRETCHED`/`LATE` only from actual scheduler deadlines/effective periods. Keep all heater actions inside Sprint 3 gates and keep direct PB11-high blocked.
4. Run the three required local checks, push, then `gh run watch` and record the successful run and artifact here. No local firmware build or flash.

## Exact next step for Sprint 5

**Do not start Sprint 5 yet.** After Sprint 4 continuous-feed and scheduler tests pass CI, Sprint 5's first task is to connect the WebUI's existing job-progress/status stream to the new asynchronous `print-job` lifecycle (queued/running/progress/terminal/cancelled) without changing the firmware safety gates. Hardware validation remains a separate owner-run step and must not be inferred from CI.
