# Sprint 0 — baseline report

Date: 2026-11-02  
Branch: `pinbridge`  
Starting HEAD: `f38307d00e5bff43bc37c928da53765cda13cf85` (`docs: split TBP work into sprint handoffs`)  
Initial worktree: clean (`git status --short --branch` showed `## pinbridge...origin/pinbridge`).

## Reproducible baseline

Commands run from the repository root unless noted:

| Command | Result |
|---|---|
| `cd host/pinbridge/bun && bun run check` | PASS — Prettier check; 6 Bun tests, 20 assertions, 0 failures (Bun 1.4.2) |
| `cd host/pinbridge && .venv/bin/python -m unittest discover -s tests -v` | PASS — 34 tests in 2.658 s |
| `./scripts/check-pinbridge.sh` | PASS — generated C/Python pin files and table current |
| `git diff --check` | PASS after documentation changes |

The Python transcript test setup compiles C test runners with GCC `-std=c11 -Wall -Wextra -Werror`; its passing tests cover the transcript fixtures and PTY simulator. This is a host-side C harness, **not** the vendor firmware build.

## Golden-vector coverage as of baseline

- CRC-16/CCITT-FALSE: Bun tests `123456789 -> 0x29B1`.
- HELLO: Bun test encodes `01010101E654`; C transcript sends the same request and checks ACK/INFO bytes.
- ROW: Bun tests a 54-byte payload's offsets and values and decodes a framed ROW; not a complete shared host/C wire vector.
- MOVE: Bun checks signed i32 two's-complement payload bytes; no matching C transcript vector.
- STAT, DWELL, FLUSH and ESTOP: no dedicated host+C shared golden wire vectors found in this baseline.

The gaps above are intentionally recorded rather than silently calling the current tests full protocol coverage. Sprint 1 should add a shared vector fixture consumed by both host and C tests as part of reliability/conformance work; the current C test runner's transcript format can support byte-exact requests and replies.

## Current implementation and mismatches

The host has TBP framing, CRC, basic command payload helpers, command ID/retry handling, UART reconnect and periodic STAT polling. C has frame parsing, HELLO, STAT, CONFIG, FLUSH, ROW, MOVE, DWELL, COILS and ESTOP dispatch. The details and ownership are in [`../tbp-implementation-matrix.md`](../tbp-implementation-matrix.md).

Most important blockers:

1. C executes ROW/MOVE/DWELL synchronously, so there is no true bounded queue and parser-dispatched ESTOP cannot interrupt an active operation.
2. Duplicate history is effectively one last accepted command, not the draft's 127-ID history; replay omits result payloads.
3. HELLO and STAT sensor/capability values are hard-coded or unknown placeholders; there are no temperature/battery/paper gates or duty-budget enforcement.
4. ROW does not implement independent step/heater timers, look-ahead shifting or continuous motion between rows; reported effective values are not measured execution telemetry.
5. CONFIG is primarily an echo path, not proof that heartbeat, idle hold and link timeout behavior are applied.
6. No complete shared host/C vectors for STAT, ROW, MOVE, DWELL, FLUSH and ESTOP are present.
7. Draft wording needs clarification on INFO payload length, duplicate replay contract, CONFIG effects, and how ESTOP can be immediate in a synchronous executor.

## CI / build boundary

The previous docs handoff recorded GitHub Actions run `41487812605` passing the Bun host checks and remote C firmware build/package. That is prior CI evidence, not a build triggered by Sprint 0. No firmware build was run locally; no flash or hardware writes were performed; no physical printer was exercised in this sprint.

## Handoff

Sprint 0 inventory is complete. Start Sprint 1 with framing/error precedence, sequential IDs and wrap, robust duplicate replay, retries/session reset, and shared host/C golden vectors. Resolve the draft ambiguities above before tests encode an irreversible interpretation. Do not change queue/executor architecture in Sprint 1; that belongs to Sprint 2.
