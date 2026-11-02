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
