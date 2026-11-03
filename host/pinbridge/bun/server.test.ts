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

describe("head temperature conversion", () => {
  test("adds Celsius using the stock table while retaining raw ADC counts", () => {
    const { link } = fakeLink(() => {});
    const status = link.parseStatusBody([
      3, 0, 4, 0, 0, 1, 114, 1, 244, 15, 160, 1, 0,
    ]);
    expect(status.headTempRaw).toBe(370);
    expect(status.headTempC).toBe(24);
    const invalid = link.parseStatusBody([
      3, 0, 4, 0, 0, 255, 255, 1, 244, 15, 160, 1, 0,
    ]);
    expect(invalid.headTempC).toBeNull();
  });
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
    const uiLogs: string[] = [];
    link.clients.add({
      send: (wire) => {
        const event = JSON.parse(wire);
        if (event.type === "log") uiLogs.push(event.message);
      },
    });
    const wire = encode(1, OP.STAT).trim();
    await link.transact(OP.STAT, new Uint8Array(), true, 8);
    link.onLine("!wdg");
    const body = out.map((l) => l.replace(/^\d\d:\d\d:\d\d\.\d{3} /, ""));
    expect(body[0]).toBe(`TX    id=1 op=02 ${wire}`);
    expect(body[1]).toBe(`TX    id=1 op=02 ${wire} (attempt 2)`);
    expect(body.some((l) => l.startsWith("RX    id=1 op=81 "))).toBe(true);
    expect(body.some((l) => l.startsWith("RX    id=1 op=82 "))).toBe(true);
    expect(body[body.length - 1]).toBe('RX?   "!wdg" (Invalid TBP hex frame)');
    expect(uiLogs).toContain('Ignored non-TBP UART line: "!wdg"');
  });
});

describe("TBP fake-transport retry behavior", () => {
  test("retries BUSY on the same ID without dropping the UART", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire, n, current) => {
      sent.push(wire);
      if (n === 1)
        current.onLine(encode(1, KIND.ERR, Uint8Array.of(9, 0)).trim());
      if (n === 2) {
        ack(current, 1);
        done(current, 1);
      }
    });
    await link.transact(OP.MOVE, Uint8Array.of(0, 0, 0, 1, 3, 232), true, 500);
    expect(count()).toBe(2);
    expect(decode(sent[0]).id).toBe(decode(sent[1]).id);
    expect(link.state).toBe("connected");
  });

  test("retries BUSY on the same ID without dropping the UART", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire, n, current) => {
      sent.push(wire);
      if (n === 1)
        setTimeout(
          () => current.onLine(encode(1, KIND.ERR, Uint8Array.of(9, 0)).trim()),
          0,
        );
      if (n === 2) {
        ack(current, 1);
        done(current, 1);
      }
    });
    await link.transact(OP.MOVE, Uint8Array.of(0, 0, 0, 1, 3, 232), true, 500);
    expect(count()).toBe(2);
    expect(decode(sent[0]).id).toBe(decode(sent[1]).id);
    expect(link.state).toBe("connected");
  });

  test("resynchronizes a concurrent sequence mismatch with STAT before retrying", async () => {
    const frames: any[] = [];
    const { link } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      frames.push(frame);
      if (frames.length === 1) {
        setTimeout(
          () => current.onLine(encode(frame.id, KIND.ERR, Uint8Array.of(6, 2))),
          0,
        );
      } else {
        ack(current, frame.id);
        done(current, frame.id);
      }
    });
    const first = link.transact(
      OP.MOVE,
      Uint8Array.of(0, 0, 0, 10, 3, 232),
      true,
      100,
    );
    const later = link.transact(OP.STAT, new Uint8Array(), true, 100);
    const laterRejected = expect(later).rejects.toThrow("sequence mismatch");
    await first;
    await laterRejected;
    expect(frames.map((frame) => frame.op)).toEqual([
      OP.MOVE,
      OP.STAT,
      OP.MOVE,
    ]);
    expect(frames.map((frame) => frame.id)).toEqual([1, 2, 3]);
    expect(link.nextId).toBe(4);
    expect(link.state).toBe("connected");
  });

  test("an ACKed command retry does not rewind the next command ID", async () => {
    const frames: any[] = [];
    const { link } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      frames.push(frame);
      ack(current, frame.id);
      if (frames.length !== 1) done(current, frame.id);
    });
    const first = link.transact(
      OP.MOVE,
      Uint8Array.of(0, 0, 0, 10, 3, 232),
      true,
      12,
    );
    const second = link.transact(OP.STAT, new Uint8Array(), true, 100);
    await second;
    await first;
    await link.transact(OP.STAT, new Uint8Array(), true, 100);
    expect(frames.map((frame) => frame.id)).toEqual([1, 2, 1, 3]);
    expect(link.nextId).toBe(4);
  });

  test("clears partial UART lines before every HELLO retransmission", async () => {
    const { link, count } = fakeLink((_wire, n, current) => {
      if (n === 2) {
        ack(current, 1);
        done(current, 1);
      }
    });
    let discarded = 0;
    (link.serial as any).discardPartialLine = () => discarded++;
    await link.transact(OP.HELLO, Uint8Array.of(1), true, 8);
    expect(count()).toBe(2);
    expect(discarded).toBe(2);
    expect(link.state).toBe("connected");
  });

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

  test("reserves one window credit for STAT while data commands fill the rest", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire) => sent.push(wire));
    const moveData = Uint8Array.of(0, 0, 0, 1, 3, 232);
    const jobs = [1, 2, 3].map(() =>
      link.transact(OP.MOVE, moveData, true, 100),
    );
    await Promise.resolve();
    await Promise.resolve();
    expect(link.pending.size).toBe(3);
    const waitingMove = link.transact(OP.MOVE, moveData, true, 100);
    const stat = link.transact(OP.STAT, new Uint8Array(), true, 100);
    await Promise.resolve();
    await Promise.resolve();
    expect(link.pending.size).toBe(4);
    expect([...link.pending.values()].some((p) => p.op === OP.STAT)).toBe(true);
    for (const id of [1, 2, 3, 4]) {
      ack(link, id);
      done(link, id);
    }
    await Promise.all([...jobs, stat]);
    await Promise.resolve();
    expect(count()).toBe(5);
    ack(link, 5);
    done(link, 5);
    await waitingMove;
    expect(link.pending.size).toBe(0);
  });

  test("does not transmit later IDs after an earlier command is rejected", async () => {
    const { link, count } = fakeLink((wire, _n, current) => {
      const id = decode(wire).id;
      if (id === 1) {
        setTimeout(() => {
          current.onLine(encode(1, KIND.ERR, Uint8Array.of(10, 0)).trim());
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
    await expect(first).rejects.toThrow("TBP ERR 0x0a");
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

  test("print-solid uses one full-duration heater pulse for timing tests", async () => {
    const frames: any[] = [];
    const { link } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      frames.push(frame);
      ack(current, frame.id);
      done(current, frame.id);
    });
    link.info = {
      queueDepth: 4,
      maxWindow: 4,
      burnTickUs: 1,
      burnMaxUs: 10000,
      guardUs: 100,
      dutyWindowMs: 200,
      dutyMaxUs: 60000,
      shiftUs: 3000,
      tempLimitRaw: 600,
      battMinRaw: 3400,
      caps: 0x0f,
    } as any;
    const result = await link.command("print-solid", {
      rows: 1,
      burnUs: 8000,
      distanceMm: 0.25,
      speedMmS: 5.2083,
    });
    expect(result).toEqual({ queued: true, rows: 1, stopped: false });
    expect(frames).toHaveLength(1);
    expect(frames[0].op).toBe(OP.ROW);
    expect(frames[0].data[48]).toBe(0x1f);
    expect(frames[0].data[49]).toBe(0x40);
    expect(frames[0].data[53]).toBe(1);
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

  test("keeps the UART open after a protocol-level rejection", async () => {
    const { link, count } = fakeLink((_wire, _n, current) => {
      setTimeout(
        () => current.onLine(encode(1, KIND.ERR, Uint8Array.of(10, 0)).trim()),
        0,
      );
    });
    await expect(
      link.transact(OP.MOVE, Uint8Array.of(0, 0, 0, 1, 3, 232), true, 100),
    ).rejects.toThrow("TBP ERR 0x0a");
    expect(count()).toBe(1);
    expect(link.state).toBe("connected");
    expect(link.serial?.isOpen).toBe(true);
  });

  test("resynchronizes next ID and releases the sender lock after an ambiguous post-ACK sequence error", async () => {
    const sent: number[] = [];
    const { link } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      sent.push(frame.id);
      setTimeout(() => ack(current, frame.id), 0);
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
    await link.transact(OP.STAT, new Uint8Array(), false, 100);
    expect(sent).toEqual([1, 5]);
    expect(link.nextId).toBe(6);
    expect(link.state).toBe("connected");
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

  test("stops after three sends but retains the UART connection", async () => {
    const { link, count } = fakeLink(() => {});
    await expect(
      link.transact(OP.STAT, new Uint8Array(), true, 5),
    ).rejects.toThrow("TBP ACK timeout; link retained");
    expect(count()).toBe(3);
    expect(link.pending.size).toBe(0);
    expect(link.state).toBe("connected");
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
