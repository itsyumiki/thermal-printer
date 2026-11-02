# Sprint 0 — Establish the baseline

**Size:** XS / one focused documentation-and-test pass  
**Purpose:** Give the next agent a reliable picture of what exists before protocol changes begin.  
**Status:** Planned; current repo has a first TBP implementation and host tests, but not full v1 conformance.

## Read first

- `docs/thermal-protocol-spec.md` (especially sections 3–10, 12, 14)
- `docs/journal.md` (latest entries)
- `host/pinbridge/bun/tbp.ts`, `tbp.test.ts`, `server.ts`
- `firmware/apps/pinbridge/src/pb_core.h` and firmware adapter
- `host/pinbridge/tests/` and the existing transcript runner

## Scope

Create a testable inventory of current behavior versus TBP draft v1. Add golden-vector tests only where they are missing and cheap. Do not rewrite the parser or executor in this sprint.

## Tasks

1. Record current branch, HEAD, dirty files and latest CI run before edits.
2. Enumerate implemented host and firmware opcodes/responses against spec sections 5–10.
3. Run existing Bun checks and the existing Python/C transcript suite; record exact commands and results.
4. Add shared golden vectors for CRC `123456789 -> 0x29B1`, HELLO, STAT, ROW, MOVE, DWELL, FLUSH and ESTOP if not already covered on both host and C sides.
5. Mark every mismatch explicitly: incorrect behavior, unimplemented behavior, or spec ambiguity. Do not disguise a mock-only path as firmware support.
6. Add a concise implementation matrix to the protocol spec or a linked document.

## Acceptance criteria

- A clean baseline test report exists and can be reproduced.
- Host and C test vectors agree byte-for-byte for every vector added.
- A spec-to-implementation matrix names each missing/partial feature and its owning sprint.
- No firmware build, flash, or hardware write is performed.

## Out of scope

Queue redesign, safety policy decisions, UI redesign, timer changes, and physical print experiments.

## Handoff on completion

Record commit SHA, test commands/output summary, mismatches found, and any spec questions that block Sprint 1. Next: Sprint 1 protocol reliability.
