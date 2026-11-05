# Sprint 7 — Validation and go/no-go

**Size:** M  
**Depends on:** Sprints 0-6  
**Goal:** Answer the pulse-width question with the new foundation and decide whether the rework is releasable.

## Read first

- Every sprint handoff in this folder
- `docs/bugs/2026-11-04-heater-blank-prints-timing.md`, `docs/timer-architecture-and-usage.md`

## Tasks

1. Re-run the full `TIMETEST` suite and record the table as the new baseline.
2. With a scope on PB11 and a phase pin, run test prints at stock-equivalent pulse widths (from Sprint 0) and at larger widths. Record darkness against width.
3. Resolve the blank-print bug: either the stock-equivalent width now prints, or the remaining difference is named with evidence.
4. Restore `PB_TUNE_HEATER_SLICE_MAX_US` and `PB_TUNE_HEATER_BURN_MAX_US` to stock-derived or measured limits.
5. Run a bench checklist: continuous print, ESTOP mid-row, paper out, hot head, USB unplug mid-print.
6. Write the go/no-go with open risks.

## Acceptance criteria

- A recorded darkness-vs-width result and a statement of the working pulse width.
- Test-only limits restored, or an owner decision recorded to keep them.
- Bench checklist results with mock, CI and hardware evidence reported separately.

## Out of scope

New features.

## Handoff on completion

Include the checklist results, remaining risks and the release decision.
