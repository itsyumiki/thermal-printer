# Sprint 0 — Stock heater and step timing

**Status (2026-11-05):** Static analysis and the derived timing table are documented in section 8 of `docs/bugs/2026-11-04-heater-blank-prints-timing.md`. This is a paper/documentation milestone only: no stock PB11 or motor-phase scope capture has been taken, so the hardware portion is still open.

**Size:** S  
**Depends on:** nothing (read-only; can run alongside any other sprint)  
**Goal:** Document the derivation for the 30 us / 0.72–2.22 ms / 6 ms stock heater estimates and obtain scope evidence, so Sprint 3 and Sprint 7 have a real pulse-width target.

## Read first

- `docs/timer-architecture-and-usage.md`, `docs/bugs/2026-11-04-heater-blank-prints-timing.md`
- Stock firmware notes and disassembly under `thermal-printer/re/docs` (if present in the tree)
- `firmware/apps/pinbridge/src/pb_tuning.h` (the STOCK-tagged values)

## Tasks

1. Find the stock TIMER2 setup: CON bits (clock source, prescaler), PRD, and whether the ISR holds PB11 high or toggles it. State the resulting tick period and a derivation, not a guess.
2. Find the stock heat threshold range and cap in ticks, and what unit the quality bytes use.
3. Find the stock step cadence: the 2 ms callback, the divider (at least 3), and how heat slices gate on motor edges.
4. Check whether stock reshifts and relatches per print event, and whether the four events carry different dot data or the same row.
5. If the owner can scope: capture PB11 and one motor phase pin during a stock print, and record pulse width, count per row and the phase-to-heat offset.

## Acceptance criteria

- A written table of stock heater pulse width (min, typical, max) with the source of each number (disassembly address or scope capture).
- Each derived number is marked DERIVED or MEASURED.
- A statement of whether the 50 ms we needed to get dark is consistent with stock, and if not, what else differs.

## Out of scope

Any firmware change.

## Handoff / current result

The static derivation and proposed capture procedure are in `docs/bugs/2026-11-04-heater-blank-prints-timing.md` section 8. The values remain **DERIVED**, not measured. The owner still needs to capture stock PB11 and a motor phase, then paste the measured widths, pulse count, spacing and phase offsets into that document. Do not mark Sprint 0 fully closed until the hardware evidence is recorded. Next dependency: Sprint 3 still needs a measured pulse-width target; Sprint 1–2 timing foundation work can proceed independently.
