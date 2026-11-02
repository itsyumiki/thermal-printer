# Sprint 5 — Production host and WebUI integration

**Size:** M  
**Depends on:** Host-side work may start with mock fixtures earlier; final acceptance depends on Sprints 1–4  
**Goal:** Make the Bun host and UI reflect actual protocol state and recover cleanly from common link/user errors.

## Read first

- `host/pinbridge/bun/README.md`, `server.ts`, `tbp.ts`, `tbp.test.ts`
- `host/pinbridge/bun/public/index.html`, `app.js`, `style.css`
- Design-system reference: https://github.com/itsyumiki/design-system
- `docs/thermal-protocol-spec.md` sections 4–5, 7, 10, 12

## Tasks

1. Keep the current single Bun server/host architecture unless measurements demonstrate a clear reason to split it. Keep UI state derived from one authoritative host connection/job state.
2. Exercise serial-port enumeration, preferred-port selection, open failures, unplug/replug, repeated reconnects, stale WebSocket clients, and shutdown. Add fake serial/transport tests for these paths.
3. Ensure a reconnect creates a new HELLO session and clears stale pending jobs. Never automatically resume heater/motion work after reconnect; require an explicit new user action.
4. Track command IDs, ACKs, terminal statuses, retries, queue credits, and last errors in a single command lifecycle model. Do not treat ACK as job completion.
5. Surface queue capacity, queued/running/completed/failed/cancelled states, progress, paper/sensor unavailable states, and timing warnings using only data the firmware actually reports.
6. Add safe interaction affordances: confirm emergency stop, disable invalid/busy-only actions, prevent accidental duplicate submissions, provide a clear reconnect/port selector, copyable diagnostics, and accessible keyboard/focus states.
7. Honor reduced motion and responsive layouts. Keep LED animation as a firmware status indication, not a browser-only claim of hardware state.
8. Verify browser/API/WebSocket behavior with mock integration tests and check that bind defaults remain localhost unless authentication is added.
9. Update README and screenshots only if they can be produced without pretending mock results are physical hardware.

## Acceptance criteria

- Disconnect/reconnect tests pass without duplicate command execution or automatic job resumption.
- UI status maps correctly to protocol states, including failures and cancellation.
- User can distinguish UART disconnected, session handshaking, connected/idle, busy, stopped, and sensor unavailable.
- No heater/stepper action is enabled based on stale telemetry or an untrusted client-side state.
- Keyboard, narrow-screen, reduced-motion and mock-mode checks are recorded.
- Bun tests/format checks and CI pass.

## Out of scope

Adding remote internet exposure, authentication-free LAN control, analytics, or a broad component-framework rewrite.

## Handoff on completion

Include tested port scenarios, UI state mapping, a mock walkthrough, and known UX gaps. Next: Sprint 6 controlled validation and release readiness.
