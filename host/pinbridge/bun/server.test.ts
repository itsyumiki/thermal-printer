import { afterEach, describe, expect, test } from "bun:test";
import { PrinterLink } from "./server.ts";
import { decode, encode, KIND, OP, STATE } from "./tbp.ts";

const links: PrinterLink[] = [];
function fakeLink(
  onWrite: (wire: string, count: number, link: PrinterLink) => void,
) {
  const link = new PrinterLink();
  links.push(link);
  link.mock = false;
  link.state = "connected";
  link.info = { maxWindow: 4, queueDepth: 4 };
  let writes = 0;
  link.serial = {
    isOpen: true,
    write(wire: string, callback?: (error?: Error | null) => void) {
      writes++;
      onWrite(wire, writes, link);
      callback?.();
    },
  } as any;
  return { link, count: () => writes };
}
function ack(link: PrinterLink, id: number) {
  link.onLine(encode(id, KIND.ACK, Uint8Array.of(0)).trim());
}
function done(link: PrinterLink, id: number) {
  link.onLine(encode(id, KIND.STATUS, Uint8Array.of(STATE.DONE)).trim());
}
afterEach(() => {
  for (const link of links.splice(0)) {
    if (link.reconnectTimer) clearTimeout(link.reconnectTimer);
    link.reconnectTimer = null;
    link.stopKeepalive();
    for (const pending of link.pending.values()) {
      if (pending.timer) clearTimeout(pending.timer);
    }
    link.pending.clear();
  }
});

describe("debug frame logging", () => {
  test("is silent unless the debug flag is on", async () => {
    const out: string[] = [];
    const { link } = fakeLink((_wire, _n, current) => {
      ack(current, 1);
      done(current, 1);
    });
    link.debug = false;
    link.debugSink = (l) => out.push(l);
    await link.transact(OP.STAT, new Uint8Array(), true, 50);
    link.onLine("!wdg");
    expect(out).toEqual([]);
  });

  test("logs TX, RX, retransmits and raw non-TBP lines", async () => {
    const out: string[] = [];
    const { link } = fakeLink((_wire, n, current) => {
      if (n === 2) {
        ack(current, 1);
        done(current, 1);
      }
    });
    link.debug = true;
    link.debugSink = (l) => out.push(l);
    const wire = encode(1, OP.STAT).trim();
    await link.transact(OP.STAT, new Uint8Array(), true, 8);
    link.onLine("!wdg");
    const body = out.map((l) => l.replace(/^\d\d:\d\d:\d\d\.\d{3} /, ""));
    expect(body[0]).toBe(`TX    id=1 op=02 ${wire}`);
    expect(body[1]).toBe(`TX    id=1 op=02 ${wire} (attempt 2)`);
    expect(body.some((l) => l.startsWith("RX    id=1 op=81 "))).toBe(true);
    expect(body.some((l) => l.startsWith("RX    id=1 op=82 "))).toBe(true);
    expect(body[body.length - 1]).toBe('RX?   "!wdg" (Invalid TBP hex frame)');
  });
});

describe("TBP fake-transport retry behavior", () => {
  test("retries the identical frame when ACK is lost", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire, n, current) => {
      sent.push(wire);
      if (n === 2) {
        ack(current, 1);
        done(current, 1);
      }
    });
    const result = await link.transact(OP.STAT, new Uint8Array(), true, 8);
    expect(result.id).toBe(1);
    expect(count()).toBe(2);
    expect(sent[0]).toBe(sent[1]);
  });

  test("retries the identical frame when terminal STATUS is lost after ACK", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire, n, current) => {
      sent.push(wire);
      ack(current, 1);
      if (n === 2) done(current, 1);
    });
    const result = await link.transact(OP.STAT, new Uint8Array(), true, 8);
    expect(result.data[0]).toBe(STATE.DONE);
    expect(count()).toBe(2);
    expect(sent[0]).toBe(sent[1]);
  });

  test("fills advertised window and releases one credit on terminal status", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire) => sent.push(wire));
    const jobs = [1, 2, 3, 4].map(() =>
      link.transact(OP.STAT, new Uint8Array(), true, 100),
    );
    const fifth = link.transact(
      OP.MOVE,
      Uint8Array.of(0, 0, 0, 1, 3, 232),
      true,
      100,
    );
    await Promise.resolve();
    expect(count()).toBe(1);
    expect([...link.pending.keys()]).toEqual([1, 2, 3, 4]);
    ack(link, 1);
    expect(count()).toBe(2);
    expect(decode(sent[1]).id).toBe(2);
    done(link, 1);
    await jobs[0];
    await Promise.resolve();
    expect(link.pending.size).toBe(4);
    for (const id of [2, 3, 4, 5]) {
      ack(link, id);
      done(link, id);
    }
    expect(count()).toBe(5);
    expect(decode(sent[4]).id).toBe(5);
    await Promise.all(jobs.slice(1));
    await fifth;
    expect(link.nextId).toBe(6);
    expect(link.pending.size).toBe(0);
  });

  test("does not transmit later IDs after an earlier command is rejected", async () => {
    const { link, count } = fakeLink((wire, _n, current) => {
      const id = decode(wire).id;
      if (id === 1) {
        setTimeout(() => {
          current.onLine(encode(1, KIND.ERR, Uint8Array.of(5, 0)).trim());
        }, 0);
      }
    });
    const first = link.transact(OP.ROW, new Uint8Array(54), true, 100);
    const second = link.transact(
      OP.MOVE,
      Uint8Array.of(0, 0, 0, 1, 3, 232),
      true,
      100,
    );
    const secondResult = second.catch((error) => error);
    await expect(first).rejects.toThrow("TBP ERR 0x05");
    expect((await secondResult).message).toBe(
      "Blocked by preceding TBP command rejection",
    );
    expect(count()).toBe(1);
    expect(link.pending.size).toBe(0);
    expect(link.state).toBe("connected");
  });

  test("blocks heater-bearing rows when firmware advertises no validated sensors or limits", async () => {
    const { link, count } = fakeLink(() => {});
    link.info = {
      queueDepth: 4,
      maxWindow: 4,
      burnTickUs: 0,
      burnMaxUs: 0,
      guardUs: 0,
      dutyWindowMs: 0,
      dutyMaxUs: 0,
      shiftUs: 0,
      tempLimitRaw: 65535,
      battMinRaw: 65535,
      caps: 0,
    } as any;
    await expect(link.command("row", { burnUs: 1500 })).rejects.toThrow(
      "Heating disabled",
    );
    expect(count()).toBe(0);
  });

  test("print-job submits a multi-row payload and reports terminal progress", async () => {
    const link = new PrinterLink();
    links.push(link);
    link.mock = true;
    link.state = "connected";
    link.info = { queueDepth: 4, maxWindow: 4 } as any;
    const progress: number[] = [];
    link.clients.add({
      send: (wire) => {
        const event = JSON.parse(wire);
        if (event.type === "print-progress") progress.push(event.done);
      },
    });
    const rows = [0, 1, 2, 3, 4].map((i) => ({
      maskHex: (i % 2 ? "00" : "FF").repeat(48),
      burnUs: 0,
      steps: 1,
      periodUs: 6000,
      slices: 1,
    }));
    const result = await link.command("print-job", { rows });
    expect(result).toEqual({ done: 5, total: 5, stopped: false });
    expect(progress).toEqual([1, 2, 3, 4, 5]);
  });

  test("move converts mm and mm/s to firmware half-steps and microseconds", async () => {
    const frames: any[] = [];
    const { link } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      frames.push(frame);
      ack(current, frame.id);
      done(current, frame.id);
    });
    await link.command("move", { distanceMm: 5, speedMmS: 31.25 });
    await link.command("move", { distanceMm: -2.5, speedMmS: 15.625 });
    await link.command("move", { steps: 80 }); // raw units keep working; default 1000 us
    expect(frames.map((f) => f.op)).toEqual([0x11, 0x11, 0x11]);
    expect([...frames[0].data]).toEqual([0, 0, 0, 160, 0x03, 0xe8]); // +160 half-steps @ 1000 us
    expect([...frames[1].data]).toEqual([0xff, 0xff, 0xff, 0xb0, 0x07, 0xd0]); // -80 @ 2000 us
    expect([...frames[2].data]).toEqual([0, 0, 0, 80, 0x03, 0xe8]);
    await expect(
      link.command("move", { distanceMm: 5, speedMmS: 40 }),
    ).rejects.toThrow("mm/s");
  });

  test("coil hold is bounded: it configures idle_hold_ms before energising", async () => {
    const frames: any[] = [];
    const { link, count } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      frames.push(frame);
      ack(current, frame.id);
      done(current, frame.id);
    });
    await link.command("coils", { enabled: true, holdMs: 3000 });
    expect(count()).toBe(2);
    expect(frames[0].op).toBe(0x04); // CONFIG
    expect([...frames[0].data]).toEqual([2, 0x0b, 0xb8]); // key 2, 3000 ms
    expect(frames[1].op).toBe(0x20); // COILS
    expect([...frames[1].data]).toEqual([1]);
  });

  test("allows heatless row/feed tests without advertising heater capability", async () => {
    const { link, count } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      ack(current, frame.id);
      done(current, frame.id);
    });
    link.info = { queueDepth: 4, maxWindow: 4, burnMaxUs: 0, caps: 0 } as any;
    const result = await link.command("row", {
      burnUs: 0,
      steps: 0,
      periodUs: 1000,
      slices: 1,
    });
    expect(result.done).toBe(true);
    expect(count()).toBe(1);
  });

  test("retries a rejected sequence at the printer's expected ID", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire, n, current) => {
      sent.push(wire);
      setTimeout(() => {
        if (n === 1) {
          current.onLine(encode(1, KIND.ERR, Uint8Array.of(6, 5)).trim());
        } else {
          ack(current, 5);
          done(current, 5);
        }
      }, 0);
    });
    const result = await link.transact(OP.STAT, new Uint8Array(), true, 30);
    expect(result.id).toBe(5);
    expect(decode(sent[0]).id).toBe(1);
    expect(decode(sent[1]).id).toBe(5);
    expect(link.nextId).toBe(6);
    expect(count()).toBe(2);
  });

  test("rejects callers waiting for credit when the active command is rejected", async () => {
    const { link, count } = fakeLink((_wire, _n, current) => {
      setTimeout(() => {
        current.onLine(encode(1, KIND.ERR, Uint8Array.of(5, 0)).trim());
      }, 0);
    });
    link.info.maxWindow = 1;
    const first = link.transact(OP.STAT, new Uint8Array(), true, 100);
    const second = link.transact(
      OP.MOVE,
      Uint8Array.of(0, 0, 0, 1, 3, 232),
      true,
      100,
    );
    const secondResult = second.catch((error) => error);
    await expect(first).rejects.toThrow("TBP ERR 0x05");
    expect((await secondResult).message).toBe("UART is not connected");
    expect(count()).toBe(1);
    expect(link.state).toBe("reconnecting");
  });

  test("does not renumber an operation after ACK when terminal history is ambiguous", async () => {
    const { link } = fakeLink((_wire, _n, current) => {
      setTimeout(() => ack(current, 1), 0);
    });
    const pending = link.transact(
      OP.MOVE,
      Uint8Array.of(0, 0, 0, 1, 3, 232),
      true,
      100,
    );
    await Promise.resolve();
    await Promise.resolve();
    ack(link, 1);
    link.onLine(encode(1, KIND.ERR, Uint8Array.of(6, 5)).trim());
    await expect(pending).rejects.toThrow("execution status is ambiguous");
    expect(link.state).toBe("reconnecting");
  });

  test("same-ID ERR SEQ retry advances the next ID after success", async () => {
    const sent: string[] = [];
    const { link } = fakeLink((wire, n, current) => {
      sent.push(wire);
      setTimeout(() => {
        if (n === 1)
          current.onLine(encode(1, KIND.ERR, Uint8Array.of(6, 1)).trim());
        else {
          ack(current, 1);
          done(current, 1);
        }
      }, 0);
    });
    await link.transact(OP.STAT, new Uint8Array(), true, 30);
    expect(sent[0]).toBe(sent[1]);
    expect(link.nextId).toBe(2);
  });

  test("stops after three sends and enters reconnecting state", async () => {
    const { link, count } = fakeLink(() => {});
    await expect(
      link.transact(OP.STAT, new Uint8Array(), true, 5),
    ).rejects.toThrow("TBP ACK timeout; reconnecting");
    expect(count()).toBe(3);
    expect(link.pending.size).toBe(0);
    expect(link.state).toBe("reconnecting");
  });

  test("HELLO starts a fresh host command-ID sequence", async () => {
    const { link } = fakeLink((_wire, _n, current) => {
      ack(current, 1);
      done(current, 1);
    });
    link.nextId = 200;
    const result = await link.transact(OP.HELLO, Uint8Array.of(1), true, 20);
    expect(result.id).toBe(1);
    expect(link.nextId).toBe(2);
  });

  test("disconnect rejects pending work and clears its timer/state", async () => {
    const { link, count } = fakeLink(() => {});
    const pending = link.transact(OP.STAT, new Uint8Array(), true, 1000);
    await Promise.resolve();
    expect(link.pending.size).toBe(1);
    link.drop("simulated reconnect");
    await expect(pending).rejects.toThrow("simulated reconnect");
    expect(count()).toBe(1);
    expect(link.pending.size).toBe(0);
    expect(link.info).toBeNull();
  });
});
