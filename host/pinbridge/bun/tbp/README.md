# tbp: hex frames <-> commands

Standalone TBP v1 translator for the Pinbridge printer. Hex in, typed objects out, and back. No I/O, no
server, no UI, no dependencies: every import stays inside this folder (a test enforces that), so you can
use it from any script or copy the folder into another project. Protocol reference:
[`docs/thermal-protocol-spec.md`](../../../../docs/thermal-protocol-spec.md).

TBP is hex only. A frame is one line of uppercase hex, `<ID><OP><LEN><DATA><CRC16>`, ended by LF.
There is no plaintext command language anywhere in this module.

## Use it

From a script next to this repo:

```ts
import {
  translate,
  encodeCommand,
  decodeCommand,
  Encoder,
} from "./tbp/index.ts";
```

From another project: `bun add /path/to/repo/host/pinbridge/bun/tbp` (it is a package, `pinbridge-tbp`),
or just import by relative path. It is plain TypeScript and Bun runs it as is.

### Read any frame (either direction)

```ts
const t = translate("021106000000C80BB87028");
if (t.ok && t.direction === "to-printer") t.command; // { type: "move", steps: 200, periodUs: 3000 }
if (t.ok && t.direction === "from-printer") t.message; // { type: "status", state: "done", ... }
t.text; // "move steps=200 period=3000us"   or   "DONE id=5 row: burn 1500us period 6000us ..."
```

`translate` never throws. A bad line comes back as `{ ok: false, error }` (bad hex, length, CRC, unknown
opcode, wrong payload size).

### Build frames

```ts
const enc = new Encoder(); // hands out ids the way the spec wants: HELLO = 1, then 2, 3, ... wrapping 255 -> 1
enc.encode({ type: "hello" }); // { id: 1, line: "01010101E654\n" }
enc.encode({ type: "move", steps: 200, periodUs: 3000 }); // { id: 2, line: "021106000000C80BB87028\n" }
encodeCommand({ type: "stat" }, 7); // one frame with an explicit id
```

Commands: `hello`, `stat`, `flush`, `estop`, `config`, `row`, `move`, `dwell`, `coils` (see `commands.ts`).
Field ranges are checked while encoding and the error names the field. Heat limits are **not** checked
here; the firmware owns them.

### Pieces

| file           | what                                                                           |
| -------------- | ------------------------------------------------------------------------------ |
| `protocol.ts`  | opcodes, response kinds, states, error codes, flags (constants only)           |
| `frame.ts`     | CRC-16/CCITT-FALSE, `encode`/`decode` of one hex line, hex helpers             |
| `commands.ts`  | `Command` type, `encodeCommand`, `decodeCommand`, `Encoder`                    |
| `messages.ts`  | printer frames -> typed messages (`decodeMessage`, `decodeInfo`, `decodeStat`) |
| `describe.ts`  | one-line descriptions for logs (output only, never parsed back)                |
| `translate.ts` | `translate()`: the one-call, both-directions entry point                       |

`decodeMessage(line, { opOf })` can be told which opcode a command id was sent with, so a DONE body is
decoded by that opcode. Without it the layout is inferred from the body length.

## Test

```sh
bun test tbp
```
