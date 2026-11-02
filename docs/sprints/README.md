# Pinbridge / TBP implementation sprints

Status: sprint handoffs, 2026-11-02. Sprint 0 baseline and Sprint 1 protocol reliability are complete against their documented acceptance gates; later sprints remain planned.

## How to use these handoffs

Work one sprint at a time, in order. Before editing, read the sprint handoff, `docs/thermal-protocol-spec.md`, the current source, and the latest `docs/journal.md` entries. Re-check the current branch and working tree; do not assume the repository still matches this plan. Keep each sprint's diff focused. Update its handoff with the commit, checks, known limitations, and exact next step. Do not flash hardware unless the owner explicitly asks. Do not build firmware locally when GitHub Actions can validate it. Never claim hardware behavior from a compile or mock test.

## Latest handoff reports

- [Sprint 0 baseline report](sprint-00-report.md) — commands, test results, implementation gaps, and Sprint 1 blockers.
- [Sprint 1 completion report](sprint-01-progress.md) — protocol changes, fake-transport and C transcript tests, checks, and evidence boundaries.
- [TBP implementation matrix](../tbp-implementation-matrix.md) — draft-to-code status and known ambiguities.

## Sprint sequence

| Sprint | Goal | Deliverable / exit gate | Handoff |
|---|---|---|---|
| 0 | Establish a trustworthy protocol baseline | Host/C golden vectors and explicit implementation-vs-spec gap list | [Sprint 0](sprint-00-baseline.md) |
| 1 | Make TBP framing, session, IDs and retries spec-correct | Shared golden cases, deterministic recovery, duplicate replay tests | [Sprint 1](sprint-01-protocol-reliability.md) |
| 2 | Implement nonblocking command execution and emergency stop | Bounded FIFO, progress/terminal states, responsive ESTOP, cancellation tests | [Sprint 2](sprint-02-async-executor.md) |
| 3 | Add real telemetry and firmware safety gates | Sensor capabilities truthful; heater/motion interlocks and limits tested | [Sprint 3](sprint-03-safety-telemetry.md) |
| 4 | Implement coordinated row scheduler and continuous feed | MCU owns row timing; no host round trip per print event; timing tests | [Sprint 4](sprint-04-print-scheduler.md) |
| 5 | Harden Bun host and WebUI around real protocol behavior | Reconnect, command lifecycle, queue credits, diagnostics and UX verified | [Sprint 5](sprint-05-host-webui.md) |
| 6 | Run controlled validation and prepare a release | Reproducible CI artifact, bench checklist/results, documented go/no-go | [Sprint 6](sprint-06-validation-release.md) |

## Dependency rule

Sprint 0 baseline is complete. Sprint 1 protocol/session/retry semantics and deterministic tests are complete. Sprint 2 depends on preserving this contract while introducing asynchronous execution. Sprint 3 must define safe behavior before heater-driven printing is treated as usable. Sprint 4 depends on the executor and safety interfaces from Sprints 2–3. Sprint 5 can be developed in parallel on mock fixtures, but its final acceptance depends on the actual queue/status semantics from Sprints 1–4. Sprint 6 follows all implementation sprints.

## Project-wide constraints

- The spec is a draft and has contradictions/open decisions. Do not silently guess: label each decision and update the spec in the sprint that resolves it.
- Preserve the existing PB1/PA1/PC7 power-button and LED behavior unless the owner changes the requirements. They are separate local controls, not TBP v1 opcodes in the draft.
- Keep host-side UI convenience separate from firmware-enforced safety. A web UI limit is not a safety interlock.
- Never increase heater settings based only on a demo or mock result. The journal notes that old delay loops materially overstated real heater pulse durations; use measured, calibrated timing.
- No local firmware builds. Use the existing GitHub Actions workflow on `pinbridge`; inspect the workflow result and downloadable artifact metadata after each firmware-changing push.
- No flashing or hardware writes without explicit owner authorization. Mock, transcript, CI-build and hardware-test evidence must be reported separately.
- Keep changes bite-sized: one primary behavior, tests for it, docs update, one commit. Avoid opportunistic refactors.

## Definition of done for every sprint

1. Acceptance criteria in that handoff are either met or clearly marked blocked with evidence.
2. Host tests, C transcript tests, formatting, and generated-file checks relevant to the touched code pass.
3. `git diff --check` passes; no unrelated edits or secrets are included.
4. Handoff records commands run and results, what was not tested, commit SHA, and the next sprint's starting point.
5. Push the sprint commit to `pinbridge` and verify GitHub Actions. Do not report success until the workflow result is known.
