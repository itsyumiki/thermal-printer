import { describe, expect, test } from "bun:test";
import { PassThrough } from "node:stream";
import { startConsole } from "./console.ts";
import { PrinterLink } from "./server.ts";
import { KIND, OP, STATE, decode, encode } from "./tbp/index.ts";

function link(onWrite: (wire: string, link: PrinterLink) => void) {
  const l = new PrinterLink();
  l.mock = false;
  l.state = "connected";
  l.info = { maxWindow: 4, queueDepth: 4 };
  l.logSink = () => {};
  l.serial = {
    isOpen: true,
    write(wire: string, cb?: (e?: Error | null) => void) {
      onWrite(wire, l);
      cb?.();
    },
  } as any;
  return l;
}

describe("stdin console (hex only)", () => {
  test("a valid frame is sent exactly as typed and its reply is decoded", async () => {
    const sent: string[] = [];
    const l = link((wire, self) => {
      sent.push(wire);
      const f = decode(wire);
      self.onLine(encode(f.id, KIND.ACK, Uint8Array.of(0)).trim());
      self.onLine(
        encode(f.id, KIND.STATUS, Uint8Array.of(STATE.DONE, 0x0b, 0xb8)).trim(),
      );
    });
    const frame = encode(
      5,
      OP.MOVE,
      Uint8Array.of(0, 0, 0, 200, 0x0b, 0xb8),
    ).trim();
    const out = await l.sendHex(frame.toLowerCase());
    expect(sent).toEqual([frame + "\n"]);
    expect(out[0]).toBe(`TX ${frame}  [move steps=200 period=3000us]`);
    expect(out.some((x) => x.includes("[ACK id=5"))).toBe(true);
    expect(out.some((x) => x.includes("DONE id=5 move: period 3000us"))).toBe(
      true,
    );
    l.stopKeepalive();
  });

  test("plaintext is refused and never reaches the UART", async () => {
    const sent: string[] = [];
    const l = link((wire) => sent.push(wire));
    for (const text of [
      "stat",
      "move 200 3000",
      "help",
      "raw 0101",
      "0101 0101E654",
      "ABC",
      "01010101E65G",
    ])
      await expect(l.sendHex(text)).rejects.toThrow(/hex frames only/);
    expect(sent).toEqual([]);
  });

  test("a frame the printer accepts moves the host's next id on, so the web UI stays in step", async () => {
    const l = link((wire, self) => {
      const f = decode(wire);
      self.onLine(encode(f.id, KIND.ACK, Uint8Array.of(0)).trim());
      self.onLine(encode(f.id, KIND.STATUS, Uint8Array.of(STATE.DONE)).trim());
    });
    l.nextId = 2;
    await l.sendHex(encode(9, OP.FLUSH).trim());
    expect(l.nextId).toBe(10);
    await l.sendHex(encode(255, OP.FLUSH).trim());
    expect(l.nextId).toBe(1);
    await l.sendHex(encode(40, OP.ESTOP).trim()); // ESTOP never advances the sequence
    expect(l.nextId).toBe(1);
    l.stopKeepalive();
  });

  test("ERR SEQ from the printer sets the next id to the one it expects", async () => {
    const l = link((wire, self) => {
      const f = decode(wire);
      self.onLine(encode(f.id, KIND.ERR, Uint8Array.of(0x06, 4)).trim());
    });
    l.nextId = 2;
    const out = await l.sendHex(encode(9, OP.STAT).trim());
    expect(out.some((x) => x.includes("ERR id=9"))).toBe(true);
    expect(l.nextId).toBe(4);
    l.stopKeepalive();
  });

  test("a frame that is not valid TBP is still sent as typed; junk replies are shown", async () => {
    const sent: string[] = [];
    const l = link((wire, self) => {
      sent.push(wire);
      self.onLine("!junk from firmware");
    });
    const bad = "020100000000"; // LEN says 1 data byte but there is none
    const out = await l.sendHex(bad);
    expect(sent[0]).toBe(bad + "\n");
    expect(out[0]).toContain("TBP length mismatch");
    expect(out).toContain("RX !junk from firmware");
    l.stopKeepalive();
  });

  test("no reply and no UART are reported plainly", async () => {
    const l = link(() => {});
    (l as any).serial.isOpen = false;
    await expect(l.sendHex(encode(1, OP.STAT).trim())).rejects.toThrow(
      /no open UART/,
    );
  });

  test("lines run in order; errors are printed with a bang; comments and blanks are skipped", async () => {
    const l = link((wire, self) => {
      const f = decode(wire);
      self.onLine(encode(f.id, KIND.ACK, Uint8Array.of(0)).trim());
      self.onLine(encode(f.id, KIND.STATUS, Uint8Array.of(STATE.DONE)).trim());
    });
    const input = new PassThrough();
    const printed: string[] = [];
    let ended!: () => void;
    const done = new Promise<void>((r) => (ended = r));
    startConsole(l, input, (x) => printed.push(x), ended);
    input.write(`frobnicate\n# comment\n\n${encode(2, OP.FLUSH).trim()}\n`);
    input.end();
    await done;
    expect(printed[0]).toMatch(/^! hex frames only/);
    expect(printed[1]).toContain("[flush]");
    l.stopKeepalive();
  });
});
