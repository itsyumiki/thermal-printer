import { describe, expect, test } from "bun:test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import {
  Encoder,
  OP,
  decodeCommand,
  decodeMessage,
  describeLine,
  encodeCommand,
  encodeRaw,
  translate,
  type Command,
} from "./index.ts";

// Golden frames from docs/thermal-protocol-spec.md section 13.
const AA = "AA".repeat(48);
const VECTORS: Array<[string, Command, number, string]> = [
  ["HELLO", { type: "hello", version: 1 }, 1, "01010101E654"],
  [
    "ROW",
    {
      type: "row",
      mask: new Uint8Array(48).fill(0xaa),
      burnUs: 1500,
      steps: 8,
      periodUs: 6000,
      slices: 4,
    },
    5,
    `051036${AA}05DC08177004366D`,
  ],
  [
    "MOVE +200",
    { type: "move", steps: 200, periodUs: 3000 },
    6,
    "061106000000C80BB8ECC7",
  ],
  [
    "MOVE -200",
    { type: "move", steps: -200, periodUs: 3000 },
    6,
    "061106FFFFFF380BB84A26",
  ],
  ["DWELL", { type: "dwell", ms: 500 }, 7, "07120201F472DD"],
  ["FLUSH", { type: "flush" }, 8, "080300306E"],
  ["ESTOP", { type: "estop" }, 9, "09FF0051F2"],
];

describe("command <-> hex", () => {
  for (const [name, cmd, id, hex] of VECTORS) {
    test(`${name} encodes to the documented frame`, () =>
      expect(encodeCommand(cmd, id)).toBe(hex + "\n"));
    test(`${name} decodes back to the same command`, () => {
      const back = decodeCommand(hex);
      expect(back.id).toBe(id);
      expect(back.command.type).toBe(cmd.type);
      expect(encodeCommand(back.command, id)).toBe(hex + "\n");
    });
  }
  test("decodeCommand rejects bad length and unknown opcodes", () => {
    expect(() =>
      decodeCommand(encodeCommand({ type: "stat" }, 1).replace("0102", "0102")),
    ).not.toThrow();
    expect(() =>
      decodeCommand(encodeRaw(1, OP.DWELL, Uint8Array.of(1))),
    ).toThrow(/needs 2/);
    expect(() => decodeCommand(encodeRaw(1, 0x55))).toThrow(/Unknown opcode/);
  });
  test("signed fields survive the round trip", () => {
    const row = decodeCommand(
      encodeCommand(
        { type: "row", mask: "black", burnUs: 0, steps: -8, periodUs: 2000 },
        3,
      ),
    ).command;
    expect(row).toMatchObject({
      type: "row",
      steps: -8,
      periodUs: 2000,
      burnUs: 0,
    });
    expect(
      decodeCommand(
        encodeCommand({ type: "move", steps: -123456, periodUs: 1000 }, 2),
      ).command,
    ).toMatchObject({ steps: -123456 });
  });
  test("range errors name the field", () => {
    expect(() => encodeCommand({ type: "dwell", ms: 0 }, 1)).toThrow(/DWELL/);
    expect(() =>
      encodeCommand({ type: "move", steps: 0, periodUs: 1000 }, 1),
    ).toThrow(/zero/);
    expect(() => encodeCommand({ type: "stat" }, 0)).toThrow(/out of range/);
  });
});

describe("Encoder id allocation", () => {
  test("HELLO is id 1 and restarts the sequence; ESTOP does not advance it", () => {
    const e = new Encoder();
    expect(e.encode({ type: "hello" })).toEqual({
      id: 1,
      line: "01010101E654\n",
    });
    expect(e.encode({ type: "stat" }).id).toBe(2);
    expect(e.encode({ type: "estop" }).id).toBe(3);
    expect(e.encode({ type: "stat" }).id).toBe(3);
    e.nextId = 255;
    expect(e.encode({ type: "stat" }).id).toBe(255);
    expect(e.encode({ type: "stat" }).id).toBe(1);
    expect(e.encode({ type: "hello" }).id).toBe(1);
    expect(e.nextId).toBe(2);
  });
});

describe("hex -> message", () => {
  test("ACK", () =>
    expect(decodeMessage("0581010337BD")).toEqual({
      type: "ack",
      id: 5,
      queueUsed: 3,
    }));
  test("QUEUED", () =>
    expect(decodeMessage("058203010203D1AF")).toMatchObject({
      type: "status",
      state: "queued",
      id: 5,
      pos: 2,
      queueUsed: 3,
    }));
  test("ROW DONE without context (layout inferred from length)", () => {
    const m: any = decodeMessage("05820C0303100A201F0E05DC1770009E38");
    expect(m.state).toBe("done");
    expect(m.result).toEqual({
      kind: "row",
      row: {
        headTempRaw: 0x0310,
        batteryRaw: 0x0a20,
        batteryMv: 7950,
        burnEffUs: 1500,
        periodEffUs: 6000,
        flags: 0,
      },
    });
  });
  test("DONE layout follows the originating opcode when the caller knows it", () => {
    // [03][00 02]: a MOVE period, but a FLUSH count of 0 followed by garbage if misread.
    const frame = encodeRaw(9, 0x82, Uint8Array.of(3, 0x0b, 0xb8));
    expect(decodeMessage(frame.trim(), { opOf: () => OP.MOVE })).toMatchObject({
      result: { kind: "move", periodEffUs: 3000 },
    });
  });
  test("ERR SEQ names the code", () =>
    expect(decodeMessage("0A830206058525")).toMatchObject({
      type: "err",
      id: 10,
      codeName: "SEQ",
      arg: 5,
    }));
  test("CANCELLED and flushed DONE", () => {
    expect(decodeMessage("06820205025D0E")).toMatchObject({
      state: "cancelled",
      reasonName: "ESTOP",
    });
    expect(decodeMessage("09820203029251")).toMatchObject({
      result: { kind: "flushed", flushed: 2 },
    });
  });
  test("heartbeat event", () => {
    const m: any = decodeMessage("00840E01020320050403100A201F0E010CE9F1");
    expect(m).toMatchObject({ type: "event", event: "heartbeat", id: 0 });
    expect(m.stat).toMatchObject({
      state: 2,
      queueUsed: 3,
      queueDepth: 32,
      runningId: 5,
      lastTerminalId: 4,
      headTempRaw: 0x0310,
      batteryMv: 7950,
      paper: 1,
      flags: 0x0c,
    });
  });
  test("a corrupted frame throws", () =>
    expect(() => decodeMessage("0581010337BE")).toThrow(/CRC/));
});

describe("describe", () => {
  test("describeLine annotates valid frames and passes other text through", () => {
    expect(describeLine("0581010337BD\r")).toBe(
      "0581010337BD  [ACK id=5 queue_used=3]",
    );
    expect(describeLine("!wdg")).toBe("!wdg");
    expect(describeLine("05820C0303100A201F0E05DC1770009E38")).toContain(
      "DONE id=5 row: burn 1500us period 6000us",
    );
  });
});

describe("translate()", () => {
  test("a command frame comes back typed, with its id and a description", () => {
    const t = translate("021106000000c80bb87028\n");
    expect(t).toMatchObject({
      ok: true,
      direction: "to-printer",
      hex: "021106000000C80BB87028",
      id: 2,
      command: { type: "move", steps: 200, periodUs: 3000 },
      text: "move steps=200 period=3000us",
    });
  });

  test("every golden command frame translates to its command and back", () => {
    for (const [, cmd, id, hex] of VECTORS) {
      const t = translate(hex);
      expect(t.ok && t.direction === "to-printer" && t.id).toBe(id);
      expect(
        t.ok &&
          t.direction === "to-printer" &&
          encodeCommand(t.command, id).trim(),
      ).toBe(hex);
    }
  });

  test("a printer frame comes back as a message", () => {
    const t = translate("0581010337BD");
    expect(t).toMatchObject({
      ok: true,
      direction: "from-printer",
      message: { type: "ack", id: 5, queueUsed: 3 },
      text: "ACK id=5 queue_used=3",
    });
  });

  test("opOf picks the DONE layout", () => {
    const t = translate("05820C0303100A201F0E05DC1770009E38", {
      opOf: () => OP.ROW,
    });
    expect(t.ok && t.direction === "from-printer" && t.message).toMatchObject({
      type: "status",
      state: "done",
      result: { kind: "row" },
    });
  });

  test("never throws: plaintext, bad CRC and unknown opcodes are reported", () => {
    expect(translate("stat")).toMatchObject({ ok: false });
    expect(translate("")).toMatchObject({ ok: false });
    expect(translate("0581010337BE")).toMatchObject({
      ok: false,
      error: expect.stringMatching(/CRC/),
    });
    const unknown = encodeRaw(3, 0x55).trim();
    const t = translate(unknown);
    expect(t).toMatchObject({
      ok: false,
      error: expect.stringMatching(/Unknown opcode 0x55/),
    });
    expect(t.ok === false && t.frame?.id).toBe(3);
  });
});

describe("standalone", () => {
  test("no module in this folder imports anything from outside it", () => {
    const dir = import.meta.dir;
    const files = readdirSync(dir).filter(
      (f) => f.endsWith(".ts") && !f.endsWith(".test.ts"),
    );
    expect(files.length).toBeGreaterThan(5);
    for (const f of files) {
      const imports = [
        ...readFileSync(join(dir, f), "utf8").matchAll(/from\s+"([^"]+)"/g),
      ].map((m) => m[1]!);
      for (const spec of imports) expect(spec).toMatch(/^\.\/[a-z]+\.ts$/);
    }
  });

  test("the package entry exports the whole API", async () => {
    const api = await import("./index.ts");
    for (const name of [
      "translate",
      "encodeCommand",
      "decodeCommand",
      "decodeMessage",
      "Encoder",
      "describeMessage",
    ])
      expect(typeof (api as Record<string, unknown>)[name]).toBe("function");
  });
});
