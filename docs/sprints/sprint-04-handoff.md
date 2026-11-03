# Sprint 4 handoff — scheduler checkpoint

Status: **scheduler implementation checkpoint; awaiting CI for this follow-up.** This is software-only work. No local firmware build, flash, or hardware validation was performed.

## Commits

- `2c0a86b25e18094480d6d5ae84196ceca75476d4` — design decisions and truthful `COLD_START` result flag; transcript expectations updated.
- `20a6e9cfbcd46089d6c7a6cbe45bde91f7273029` — Bun `print-job` endpoint and demo submits one multi-row request, consumes row progress and terminal completion.
- `8593b53e76db4e5dd9c0ff4f7764a362a234ac46` — firmware feed segment ring, buffered terminal statuses, accurate fake-clock delay model, and scheduler transcripts.
- `a67b1262a6ecf3e8aef3cd7413a320a30f337ad9` — initial handoff/journal update.

## Checks

Passed locally on the checkpoint:

- `cd host/pinbridge/bun && bun run check` — Prettier and Bun tests: 22 passed, 79 assertions.
- `./scripts/check-pinbridge.sh` — generated files OK.
- `git diff --check` — passed.
- `host/pinbridge/.venv/bin/python -m unittest discover -s host/pinbridge/tests -v` — 35 tests passed.
- `python3 -m py_compile host/pinbridge/examples/print_matrix_square_ws_demo.py` — passed.
- Prior checkpoint GitHub Actions [run 41587395888](https://github.com/itsyumiki/thermal-printer/actions/runs/41587395888) passed host checks and remote C firmware build/package. Artifact: `pinbridge-br23-41587395888` (663,477 bytes). It predates the scheduler segment-ring follow-up; current CI run/artifact pending push. CI is not hardware validation.

These checks establish software/mock behavior only. They do not establish physical feed direction, dot polarity, thermal safety, actual shift time, or continuous motion.

## Stock behavior: confirmed vs inferred

- **Confirmed by static stock-firmware analysis, not hardware measurement:** nominal 2 ms callback; divider at least 3 implies nominal 6 ms event-post interval before queueing; a stock logical row is processed as four same-mask shift/latch/heat events and four stepper phase advances; stock shift order is byte 0..47 and bit 0..7 (LSB first); the eight-state half-step sequence and head/stepper pin roles are described in the timing reference.
- **Confirmed in Pinbridge source/spec:** PB10 DATA, PB9 CLK, PB8 active-low latch, PB11 heater strobe; TBP bit 7 of byte 0 is the logical leftmost dot; positive TBP steps mean forward feed; phase index is persistent across commands; heat still goes through Sprint 3 gates.
- **Inferred/unverified:** the shift register is isolated from the currently latched head image; actual dot-to-wire mapping and black polarity; physical forward direction; exact head-driver electrical timings; stock ~30 us heater tick derived from timer setup; 3 ms shift estimate. No scope or paper-motion measurements exist.
- **Decision:** do not pre-shift the next row until register isolation is proved. `PB_TUNE_HEAD_SHIFT_TIME_US=3000` remains a planning estimate only. Timer-based motor service remains millisecond-granularity in the current portable executor; no sub-ms precision is claimed.

## What this checkpoint changes

- ROW feed is now scheduled before shift/latch/burn so the timer-driven motor work overlaps row preparation. Compatible queued ROW/MOVE commands (same direction and effective millisecond period) extend the active phase train; terminal DONE frames are buffered until each command's cumulative phase-transition target is reached. The phase index and coil outputs are not reset at those compatible boundaries.
- A ROW reports `COLD_START` when its movement must begin with no active phase train. It reports `LATE` when its own feed quota has already completed before shift/latch/burn reaches the motion handoff. `STRETCHED` remains clear because the executor does not yet implement period stretching.
- The Bun host accepts one `print-job` request (up to 2000 ROWs), pipelines within the advertised queue depth, emits progress after terminal ROW statuses, and returns a final count. The demo builds one job with four identical TBP ROW events per logical image row and retains dry-run mode.
- The deterministic fake-HAL transcript `tbp-row-continuous.txt` checks row-to-row and row-to-MOVE handoff, cumulative step accounting, phase/direction, no-heat flags, and empty-queue completion. `tbp-row-estop-boundary.txt` checks ESTOP cancellation of both a pending terminal and the active row. The fake delay now accumulates microseconds accurately rather than treating every sub-millisecond GPIO wait as a full millisecond.
- The feed scheduler now uses a bounded phase-segment ring for queued ROW/MOVE commands. Direction and effective-period changes are applied at phase boundaries without releasing coils or restarting the phase train. The fake-HAL transcript verifies a queued ROW/ROW/REV-MOVE sequence across both a direction and period change.

## Throughput limits

No measured throughput is available. Feed segments overlap shift/latch/heat with the active motor phase train, including queued direction/effective-period changes at phase boundaries. Pre-shift remains disabled. The stock 6 ms event cadence and 3 ms shift time are static estimates, not Pinbridge timing guarantees. The demo's four-event representation does not prove stock-equivalent dot placement or thermal behavior.

## Required work before Sprint 4 can be called complete

1. Review the new scheduler code and transcripts for any remaining edge cases around segment-ring exhaustion and cancellation during a direction/period boundary. The current ring is bounded by TBP queue depth and enqueue credit accounting.
2. The deterministic C transcripts now cover four events for one logical row, ROW/ROW and ROW/MOVE handoff, direction/period changes, forward/reverse phase order, no-heat flags, detected late shift, empty-queue completion, and ESTOP with a pending terminal. Existing gate transcripts cover sliced burns and fail-closed sensor/paper/battery behavior.
3. Keep `STRETCHED` clear unless effective-period extension is explicitly detected; keep `LATE` tied to feed quota completion before row preparation handoff. Keep all heater actions inside Sprint 3 gates and keep direct PB11-high blocked.
4. Run the three required local checks, push, then `gh run watch` and record the successful run and artifact here. No local firmware build or flash.

## Exact next step for Sprint 5

**Do not start Sprint 5 yet.** After Sprint 4 continuous-feed and scheduler tests pass CI, Sprint 5's first task is to connect the WebUI's existing job-progress/status stream to the new asynchronous `print-job` lifecycle (queued/running/progress/terminal/cancelled) without changing the firmware safety gates. Hardware validation remains a separate owner-run step and must not be inferred from CI.
