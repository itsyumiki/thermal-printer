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

## Known issues and oddities to resolve before release

Collected during Sprint 3-4 review on 2026-11-03. None of these were verified on hardware.

1. **Boot-time stepper drive (fixed in software, needs bench confirmation).** `pb_init` used to drive PA9/PA10/PC3/PC2 to phase 0, energizing both DRV8833 bridges at boot until the 5 s watchdog released them. They are now released (high-Z) at init. Confirm on the bench that the DRV8833 inputs have pull-downs and that the coils really are unpowered after boot, and that no phase pin glitches while the SDK starts.
2. **Power-on wiggle is new and unmeasured.** After the 1 s power-button hold, the motor does 200 half-steps forward then 200 back at 6 ms each (`PB_TUNE_BOOT_WIGGLE_*` in `pb_tuning.h`, 0 steps disables). The stock firmware's wiggle count/timing was not recovered from the RE docs; 200 half-steps and 6 ms are owner/provisional values. It is cancelled by HELLO, ESTOP, FLUSH, watchdog, USB detach and shutdown, so a host that connects within about 2.4 s of power-on cancels it. Verify the paper really returns to its start position and the motor does not skip.
3. **Coil-hold policy is not applied by `print-job`.** `idle_hold_ms` defaults to 0 and HELLO resets it to 0. Only the Bun `coils` and `config` actions send it. If the queue drains mid-job (host stall, WebSocket jitter) the coils release, then re-energize at the remembered phase, which can slip paper. Either send CONFIG `idle_hold_ms` after HELLO in `print-job`, or raise the firmware default once the coil thermal limit is measured. The queue only buffers about 48 ms of rows at 2 half-steps x 6 ms.
4. **Unwanted-stop detection is indirect.** The host can only see it through the ROW DONE `COLD_START` flag and the motor-outputs-driven bit in STAT; the firmware has no current sensing. Consider counting `COLD_START` mid-job in the host and surfacing it.
5. **Demo uses 2 half-steps per event**, stock is 1. Only the 4-events-per-row structure is from stock analysis; dot placement and thermal behavior are not equivalent.
6. **Heater limits are owner decisions, not measurements** (per-pulse 2500 us, ROW 10000 us, 30% duty, 70/75/50 C, 3400 mV). The 3 ms shift time and ~30 us stock heat tick are estimates. `STRETCHED` is never set.
7. **Stale or inconsistent docs.** `scratch-bridge/README.md` still describes the BLE/SPP experiments. The PHEAT/PEVENT legacy path and the TBP path share gates but are two code paths; consider removing the legacy heat commands before release.
8. **Housekeeping.** `~/projects/scratch-bridge` holds many `build-*`/`sdk-*` scratch dirs and logs from the 2026-11-02 experiments; clean up (ask before deleting).
9. **Unauthenticated WebUI.** The Bun host binds to localhost by default and has no auth. Never expose it publicly with a real printer attached and the heater enabled.
