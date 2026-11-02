# thermal-printer

Open-source firmware, reverse-engineering notes, and host tools for inexpensive JieLi-based toy thermal printers.

## Pinbridge

Pinbridge firmware exposes a USB-CDC interface for printer GPIO and printhead control. The production browser UI and UART/TBP host are implemented in Bun/TypeScript:

```sh
cd host/pinbridge/bun
bun install --frozen-lockfile
bun run start
```

Open `http://127.0.0.1:8765`. See [`host/pinbridge/bun/README.md`](host/pinbridge/bun/README.md) and [`docs/thermal-protocol-spec.md`](docs/thermal-protocol-spec.md) for setup and protocol details. Firmware builds run through the repository's GitHub Actions workflow; do not flash without separately reviewing the artifact and safety limits.
