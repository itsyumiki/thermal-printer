# Documentation index

| Document | What it covers |
|---|---|
| [quickstart.md](quickstart.md) | End-to-end procedure: set up the build host, build, make an image, flash, verify, restore |
| [project-layout.md](project-layout.md) | Every directory in the workspace, what is generated, what to edit, how to add an app |
| [hardware-and-flash.md](hardware-and-flash.md) | The chip, how it boots, the chip key, and a byte-level map of the original flash dump |
| [build-system.md](build-system.md) | Toolchain, pinned SDK, how an app is applied to the SDK, why the SDK's own post-build is bypassed, reproducibility |
| [flashing.md](flashing.md) | Getting the chip into UBOOT mode, the exact write/read-back/restore commands, what is and isn't verified |
| [image-patcher.md](image-patcher.md) | How `tools/patch_app.py` works, what was checked, and its limits |
| [troubleshooting.md](troubleshooting.md) | Every failure hit so far, with cause and fix |
| [stock-print-scheduler-handoff.md](stock-print-scheduler-handoff.md) | Recovered stock print/stepper cadence, implementation direction, acceptance criteria, and next-agent handoff |
| [thermal-protocol-spec.md](thermal-protocol-spec.md) | Draft v1 binary host/printer protocol: framing, ids, opcodes, queue, row timing model, safety limits, error codes, examples |
| [sprints/README.md](sprints/README.md) | Bite-sized implementation plan, dependencies, definition of done, and per-sprint agent handoffs |
| [../host/pinbridge/bun/README.md](../host/pinbridge/bun/README.md) | Bun-based production WebSocket/UART host, TBP helpers, browser UI, and current executor limitations |
| [journal.md](journal.md) | What was tried, in order, including dead ends, and what is not done yet |
| [archive/](archive/) | Early research notes, superseded |

Conventions: **"verified"** means it was run and the result was observed. **"Inferred"** means it follows from code or data but was not tested. Hashes quoted are SHA-256.
