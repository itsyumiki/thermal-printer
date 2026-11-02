# Sprint 6 — Validation, evidence and release readiness

**Size:** M (includes owner-run bench time)  
**Depends on:** Sprints 0–5  
**Goal:** Produce a traceable, conservative go/no-go assessment and a reproducible CI artifact. This sprint does not authorize flashing or physical testing by itself.

## Read first

- All sprint handoffs and their completion notes
- `docs/thermal-protocol-spec.md` section 14
- `docs/flashing.md`, `docs/hardware-and-flash.md`, `docs/troubleshooting.md`
- Latest GitHub Actions run and artifact/checksum metadata

## Tasks

1. Audit acceptance criteria from Sprints 0–5. Any unclosed safety-critical gap blocks release; do not waive it silently.
2. Confirm clean checkout reproducibility through the existing GitHub Actions workflow. Record commit, workflow URL/run ID, artifact name, firmware hash, and host test results.
3. Prepare a staged bench checklist that starts with power-off inspection and logic-only tests, then UART/session tests, motor-only tests, sensor validation, and finally controlled heater tests only after the owner authorizes them.
4. Define expected result, abort condition, measurement instrument, and evidence for each hardware test. Include ESTOP latency, UART unplug behavior, button hold, LED states, paper sensor, temperature/battery readings, row alignment, continuous feed, and thermal limits.
5. Do not use black full-width test patterns or previous demo heat values as proof of safe limits. Begin heater tests with an owner-approved conservative setting and a verified temperature/duty gate.
6. Validate firmware artifact provenance and checksum before any owner-run flash. Do not flash or publish an artifact on the agent's own initiative.
7. Record every result as one of: automated test, CI build, mock integration, bench measurement, or user-reported hardware result. Keep these evidence classes separate.
8. Update the main protocol status from draft only when conformance gaps are resolved; otherwise explicitly call the artifact experimental and list blocked items.

## Acceptance criteria

- Clean CI build and host tests pass on the final commit; artifact and checksum are traceable to that commit.
- Every spec requirement is marked implemented/tested, implemented/not hardware-tested, unimplemented, or intentionally deferred.
- A bench plan includes safety prerequisites and stop conditions and is reviewed by the owner before physical testing.
- No release claim implies a flash or hardware test that did not occur.
- Final handoff includes go/no-go recommendation and remaining risks.

## Out of scope

Unapproved flashing, production release claims, automatic public artifact publication, and print-quality optimization beyond the agreed acceptance target.

## Final handoff

Summarize the exact commit/artifact, all evidence types, known risks, owner decisions still required, and a clear go/no-go recommendation.
