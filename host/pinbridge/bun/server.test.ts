import { afterEach, describe, expect, test } from "bun:test";
import { PrinterLink, summarizeTimeRuns, TIME_TEST_SOURCES } from "./server.ts";
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

  test("print-solid pipelines one latched ROW per row with four synchronized slices", async () => {
    const frames: any[] = [];
    const ids: number[] = [];
    let writesBeforeFirstDone = 0;
    let firstDoneSent = false;
    const { link } = fakeLink((wire, _n, current) => {
      const frame = decode(wire);
      frames.push(frame);
      ids.push(frame.id);
      ack(current, frame.id);
      if (frames.length === 3 && !firstDoneSent) {
        writesBeforeFirstDone = frames.length;
        firstDoneSent = true;
        for (const id of ids) done(current, id);
      } else if (firstDoneSent) {
        done(current, frame.id);
      }
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
    const result = await link.command("print-solid", { rows: 4, burnUs: 8000 });
    expect(result).toEqual({ done: 4, total: 4, stopped: false });
    expect(writesBeforeFirstDone).toBe(3);
    expect(frames).toHaveLength(4);
    expect(frames.every((frame) => frame.op === OP.ROW)).toBe(true);
    for (const frame of frames) {
      expect(frame.data[48]).toBe(0x1f); // 8000 µs total row heat, four 2000 µs slices
      expect(frame.data[49]).toBe(0x40);
      expect(frame.data[50]).toBe(4); // four motor edges per logical row
      expect(frame.data[51]).toBe(0x17);
      expect(frame.data[52]).toBe(0x70); // 6000 µs per half-step
      expect(frame.data[53]).toBe(4);
    }
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

describe("text diagnostics", () => {
  test("PDIAG sends the plain-text command and resolves with the firmware reply line", async () => {
    const sent: string[] = [];
    const { link } = fakeLink((wire, _n, l) => {
      sent.push(wire);
      queueMicrotask(() =>
        l.onLine("ok diag step=0 phase=3 done=9 rem=0 ticks=1234"),
      );
    });
    const r = await link.command("diag", { cmd: "PDIAG" });
    expect(sent).toEqual(["PDIAG\n"]);
    expect(r.reply).toContain("ticks=1234");
  });

  test("PHDIAG matches only its own reply and a firmware error rejects", async () => {
    const { link } = fakeLink((wire, _n, l) => {
      queueMicrotask(() => {
        l.onLine("ok diag step=0"); // unrelated line is not taken as the answer
        l.onLine(
          wire.startsWith("PHDIAG")
            ? "ok hdiag starts=1 isr=0 poll=1 over=0 cnt=1 con=25"
            : "err bsy x",
        );
      });
    });
    expect((await link.command("diag", { cmd: "PHDIAG" })).reply).toContain(
      "poll=1",
    );
    await expect(link.command("diag", { cmd: "PSTEPSTAT" })).rejects.toThrow(
      /err bsy/,
    );
  });

  test("only whitelisted read-only commands can be sent", async () => {
    const { link, count } = fakeLink(() => {});
    await expect(link.command("diag", { cmd: "SET PB11 1" })).rejects.toThrow(
      /Unknown diagnostic/,
    );
    expect(count()).toBe(0);
  });
});

describe("timer self-test (PTIME)", () => {
  const T3_END =
    "ok ptime T3D16 n=25 want=1000000 dj=100 ticks=1500000 div=16 isr=0";
  test("averages the host wall time between the begin and end lines", async () => {
    const walls = [1000, 1010, 990];
    const sent: string[] = [];
    let run = 0;
    let t = 0;
    const { link } = fakeLink((wire, _n, l) => {
      sent.push(wire);
      const wall = walls[run++];
      queueMicrotask(() => {
        t = 5000 * run; // each run starts at a different absolute time
        l.onLine("ptime begin T3D16 want=1000000");
        t += wall;
        l.onLine(T3_END);
      });
    });
    link.clock = () => t;
    const r = await link.command("time-test", { src: "T3D16", reps: 3 });
    expect(sent).toEqual(Array(3).fill("PTIME T3D16 1000\n"));
    expect(r.runs).toBe(3);
    expect(r.wantMs).toBe(1000);
    expect(r.meanWallMs).toBeCloseTo(1000, 6);
    expect(r.stdevMs).toBeCloseTo(10, 6);
    expect(r.ratio).toBeCloseTo(1, 6);
    // 1.5e6 ticks at /16 in 1 s is a 24 MHz oscillator; jiffy cross-check is wall / dj = 10 ms.
    expect(r.impliedOscMHz).toBeCloseTo(24, 1);
    expect(r.msPerJiffy).toBeCloseTo(10, 6);
  });

  test("a slow timer shows up in the ratio and the implied clock", () => {
    const run = (wallMs: number) => ({
      wallMs,
      n: 25,
      wantUs: 1_000_000,
      dj: 100,
      ticks: 1_500_000,
      div: 16,
      isr: 0,
    });
    const r = summarizeTimeRuns("T3D16", [run(4000), run(4000)]);
    expect(r.ratio).toBeCloseTo(4, 6);
    expect(r.impliedOscMHz).toBeCloseTo(6, 6); // really a 6 MHz oscillator
    const j = summarizeTimeRuns("JIFFY", [
      { wallMs: 2000, n: 100, wantUs: 1_000_000, dj: 100, ticks: null, div: null, isr: null },
    ]);
    expect(j.msPerUnit).toBeCloseTo(20, 6); // a jiffy is 20 ms, not 10
    expect(j.msPerJiffy).toBeUndefined();
  });

  test("a compare counted through both the ISR and the pending flag does not inflate the implied clock", () => {
    // Pre-fix firmware at /16: 25 counted, only 16 real compares (16 x 40 ms = 640 ms, a true 24 MHz clock).
    const old = summarizeTimeRuns("T3D16", [
      { wallMs: 640, n: 25, wantUs: 1_000_000, dj: 64, ticks: 1_500_000, div: 16, isr: 16 },
    ]);
    expect(old.impliedOscMHz).toBeCloseTo(24, 6); // 37.5 MHz if n were trusted
    expect(old.isrMatchesN).toBe(false);
    // Fixed firmware: one compare, one ISR, one count.
    const fixed = summarizeTimeRuns("T3D16", [
      { wallMs: 1000, n: 25, wantUs: 1_000_000, dj: 100, ticks: 1_500_000, div: 16, isr: 25 },
      { wallMs: 1001, n: 25, wantUs: 1_000_000, dj: 100, ticks: 1_500_000, div: 16, isr: 25 },
    ]);
    expect(fixed.isrMatchesN).toBe(true);
    expect(fixed.isrPerRun).toBe(25);
    expect(fixed.impliedOscMHz).toBeCloseTo(24, 1);
    // Sources without an ISR count report no match flag.
    const j = summarizeTimeRuns("JIFFY", [
      { wallMs: 1000, n: 100, wantUs: 1_000_000, dj: 100, ticks: null, div: null, isr: null },
    ]);
    expect(j.isrMatchesN).toBeUndefined();
  });

  test("a firmware refusal rejects with its reply and sends nothing else", async () => {
    const { link, count } = fakeLink((_w, _n, l) =>
      queueMicrotask(() => l.onLine("err bsy PTIME")),
    );
    await expect(
      link.command("time-test", { src: "STEP", reps: 2 }),
    ).rejects.toThrow(/err bsy PTIME/);
    expect(count()).toBe(1);
  });

  test("a firmware safety timeout is reported as a failure, not a measurement", async () => {
    const { link } = fakeLink((_w, _n, l) =>
      queueMicrotask(() => {
        l.onLine("ptime begin T3D4 want=1000000");
        l.onLine("err timeout PTIME T3D4 n=0 want=1000000 dj=1500 ticks=0 div=4 isr=0");
      }),
    );
    await expect(
      link.command("time-test", { src: "T3D4", reps: 1 }),
    ).rejects.toThrow(/never finished/);
  });

  test("rejects unknown sources and clamps out-of-range arguments", async () => {
    const sent: string[] = [];
    const { link, count } = fakeLink((wire, _n, l) => {
      sent.push(wire);
      queueMicrotask(() => {
        l.onLine("ptime begin DELAY want=3000000");
        l.onLine("ok ptime DELAY n=3000 want=3000000 dj=300");
      });
    });
    await expect(link.command("time-test", { src: "SET PB11=1" })).rejects.toThrow(/Unknown timer source/);
    expect(count()).toBe(0);
    await link.command("time-test", { src: "DELAY", ms: 99999, reps: 1 });
    expect(sent).toEqual(["PTIME DELAY 3000\n"]);
  });

  test("other commands are refused while a run is counting, and the keepalive resumes after", async () => {
    let l!: PrinterLink;
    const { link } = fakeLink((_w, _n, link) => {
      l = link;
    });
    const running = link.command("time-test", { src: "JIFFY", reps: 1 });
    await new Promise((r) => setTimeout(r, 5));
    expect(link.keepaliveTimer).toBeNull();
    await expect(link.command("status", {})).rejects.toThrow(/self-test is running/);
    await expect(link.command("time-test", { src: "JIFFY" })).rejects.toThrow(/already running/);
    l.onLine("ptime begin JIFFY want=1000000");
    l.onLine("ok ptime JIFFY n=100 want=1000000");
    const r = await running;
    expect(r.src).toBe("JIFFY");
    expect(link.keepaliveTimer).not.toBeNull();
  });

  test("the host source list matches the firmware's pb_time_srcs table", async () => {
    const fs = await import("node:fs");
    const printer = fs.readFileSync(
      new URL("../../../firmware/apps/pinbridge/src/pb_printer.h", import.meta.url),
      "utf8",
    );
    const body = /pb_time_srcs\[PB_TIME_NSRC\] = \{([^}]*)\}/.exec(printer)![1];
    const names = [...body.matchAll(/"([A-Z0-9]+)"/g)].map((m) => m[1]);
    expect(names).toEqual([...TIME_TEST_SOURCES]);
  });
});
