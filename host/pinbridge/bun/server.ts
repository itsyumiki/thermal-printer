import { existsSync, readFileSync } from "node:fs";
import { resolve, extname } from "node:path";
import { Uart, listPorts } from "./uart.ts";
import { motionFromMessage } from "./units.ts";
import { headTempCFromRaw } from "./thermistor.ts";
import {
  OP,
  KIND,
  STATE,
  encode,
  decode,
  decodeInfo,
  row,
  move,
  u16,
  join,
} from "./tbp.ts";

const root = resolve(import.meta.dir, "public");
const host = process.env.PINBRIDGE_HOST ?? "127.0.0.1";
const port = Number(process.env.PINBRIDGE_PORT ?? 8765);
const autoPath = process.env.PINBRIDGE_UART ?? "";
const mockMode = process.env.PINBRIDGE_MOCK === "1";
// Raw UART frame logging to stdout: PINBRIDGE_DEBUG=1 or `bun run server.ts --debug`.
const debugFrames =
  /^(1|true|yes|on)$/i.test(process.env.PINBRIDGE_DEBUG ?? "") ||
  process.argv.includes("--debug");
function debugStamp() {
  const d = new Date();
  const p = (n: number, w = 2) => String(n).padStart(w, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}
type Client = { send: (s: string) => void };
type Pending = {
  id: number;
  op: number;
  resolve: (f: any) => void;
  reject: (e: Error) => void;
  acked: boolean;
  waitTerminal: boolean;
  timer?: ReturnType<typeof setTimeout>;
  retries: number;
  timeout: number;
  retry?: (expectedId?: number) => void;
};
export class PrinterLink {
  serial: Uart | null = null;
  path = "";
  state: "disconnected" | "connecting" | "connected" | "reconnecting" =
    "disconnected";
  info: any = null;
  status: any = null;
  lastError = "";
  nextId = 1;
  pending = new Map<number, Pending>();
  creditWaiters: Array<() => void> = [];
  creditReservations = 0;
  activeSendId: number | null = null;
  outbound: Array<{ id: number; send: () => void }> = [];
  clients = new Set<Client>();
  reconnectTimer: any = null;
  keepaliveTimer: any = null;
  stopped = false;
  generation = 0;
  connectWanted = true;
  stopRequested = false;
  mock = mockMode;
  debug = debugFrames;
  debugSink: (line: string) => void = (line) => console.log(line);
  /** Debug trace line (only when the debug flag is on); never throws. */
  private dbg(tag: string, text: string) {
    if (!this.debug) return;
    try {
      this.debugSink(`${debugStamp()} ${tag.padEnd(5)} ${text}`);
    } catch {}
  }
  emit(type: string, data: any = {}) {
    const packet = JSON.stringify({ type, ...data, at: Date.now() });
    for (const c of this.clients)
      try {
        c.send(packet);
      } catch {}
  }
  setState(state: typeof this.state, extra: any = {}) {
    this.state = state;
    this.emit("connection", {
      state,
      path: this.path,
      info: this.info,
      error: this.lastError,
      ...extra,
    });
  }
  startKeepalive() {
    if (this.keepaliveTimer) clearInterval(this.keepaliveTimer);
    this.keepaliveTimer = setInterval(() => {
      if (this.state !== "connected") return;
      if (this.mock) {
        this.emit("printer", { info: this.info, status: this.status });
        return;
      }
      void this.transact(OP.STAT, new Uint8Array(), true, 1200)
        .then((f: any) => {
          if (f.op === KIND.STATUS && f.data.length >= 14)
            this.status = this.parseStatusBody([...f.data.slice(1)]);
          this.emit("printer", { info: this.info, status: this.status });
        })
        .catch((e: any) =>
          this.drop(e instanceof Error ? e.message : String(e)),
        );
    }, 1500);
  }
  stopKeepalive() {
    if (this.keepaliveTimer) clearInterval(this.keepaliveTimer);
    this.keepaliveTimer = null;
  }
  async ports() {
    if (this.mock)
      return [
        {
          path: "MOCK",
          manufacturer: "Pinbridge virtual printer",
          serialNumber: "",
          vendorId: "",
          productId: "",
        },
      ];
    try {
      return listPorts();
    } catch (e) {
      this.lastError = String(e);
      return [];
    }
  }
  async start() {
    if (this.mock) {
      await this.connect("MOCK");
      return;
    }
    if (autoPath) this.path = autoPath;
    this.scheduleReconnect(50);
  }
  scheduleReconnect(ms = 1800) {
    if (this.stopped || !this.connectWanted || this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      void this.connect();
    }, ms);
  }
  async connect(path = this.path) {
    this.connectWanted = true;
    if (this.mock) {
      this.path = "MOCK";
      // A HELLO starts a fresh protocol session; the first subsequent command is id 02.
      this.nextId = 2;
      this.info = {
        version: 1,
        buildId: "MOCK-DEV",
        dots: 384,
        queueDepth: 4,
        maxWindow: 4,
        minPeriodUs: 1000,
        stepTickUs: 1000,
        burnTickUs: 0,
        burnMaxUs: 0,
        guardUs: 0,
        dutyWindowMs: 0,
        dutyMaxUs: 0,
        shiftUs: 0,
        tempLimitRaw: 65535,
        battMinRaw: 65535,
        caps: 0,
        uid: "MOCK00000001",
      };
      this.status = {
        state: 0,
        queueUsed: 0,
        queueDepth: 4,
        runningId: 0,
        lastTerminalId: 0,
        headTempRaw: 65535,
        batteryRaw: 65535,
        batteryMv: 65535,
        paper: 255,
        flags: 0,
      };
      this.lastError = "";
      this.setState("connected");
      this.startKeepalive();
      this.emit("toast", {
        level: "success",
        message: "Virtual printer connected",
      });
      this.emit("printer", { info: this.info, status: this.status });
      return;
    }
    if (path) this.path = path;
    if (this.serial?.isOpen && this.state === "connected") return;
    const gen = ++this.generation;
    this.closePort();
    this.setState("connecting");
    try {
      const list = await this.ports();
      if (!this.path || !list.some((p) => p.path === this.path))
        this.path = autoPath || (list[0]?.path ?? "");
      if (!this.path) throw new Error("No UART device found");
      const serial = new Uart(this.path, 115200);
      serial.onLine = (line) => this.onLine(line);
      serial.onError = (e) => this.onPortError(e);
      serial.onClose = () => this.onPortClose();
      this.serial = serial;
      serial.open();
      if (gen !== this.generation) {
        serial.close();
        return;
      }
      await new Promise((r) => setTimeout(r, 120));
      const hello = await this.transact(OP.HELLO, Uint8Array.of(1), true, 1600);
      this.info = decodeInfo(hello.data);
      this.lastError = "";
      this.stopRequested = false;
      this.setState("connected");
      this.startKeepalive();
      this.emit("toast", {
        level: "success",
        message: "Printer connected · TBP v" + this.info.version,
      });
      this.emit("printer", { info: this.info, status: this.status });
    } catch (e) {
      this.lastError = e instanceof Error ? e.message : String(e);
      this.setState("reconnecting");
      this.closePort();
      this.scheduleReconnect();
    }
  }
  closePort() {
    const s = this.serial;
    this.serial = null;
    if (s?.isOpen)
      try {
        s.close();
      } catch {}
  }
  onPortError(e: Error) {
    this.lastError = e.message;
    this.dbg("ERR", "UART: " + e.message);
    this.emit("log", { level: "error", message: "UART: " + e.message });
    this.drop("UART error");
  }
  onPortClose() {
    if (this.serial) {
      this.serial = null;
      this.drop("UART closed");
    }
  }
  private async waitForCredit(op: number) {
    const depth = Math.max(1, Number(this.info?.maxWindow ?? 1));
    const control = op === OP.STAT || op === OP.ESTOP;
    const limit = control ? depth : Math.max(1, depth - 1);
    while (this.pending.size + this.creditReservations >= limit) {
      await new Promise<void>((resolve) => this.creditWaiters.push(resolve));
    }
    this.creditReservations++;
  }
  private wakeCreditWaiters(all = false) {
    if (all) {
      for (const resolve of this.creditWaiters.splice(0)) resolve();
    } else {
      this.creditWaiters.shift()?.();
    }
  }
  private queueSend(id: number, send: () => void, priority = false) {
    if (priority) this.outbound.unshift({ id, send });
    else this.outbound.push({ id, send });
    this.dispatchSend();
  }
  private dispatchSend() {
    if (this.activeSendId !== null) return;
    while (this.outbound.length) {
      const next = this.outbound.shift()!;
      if (!this.pending.has(next.id)) continue;
      this.activeSendId = next.id;
      next.send();
      return;
    }
  }
  private releaseSend(id: number, dispatch = true) {
    if (this.activeSendId === id) this.activeSendId = null;
    if (dispatch) this.dispatchSend();
  }
  drop(reason: string) {
    this.dbg("DROP", reason);
    this.stopKeepalive();
    this.generation++;
    for (const p of this.pending.values()) {
      if (p.timer) clearTimeout(p.timer);
      p.reject(new Error(reason));
    }
    this.pending.clear();
    this.outbound = [];
    this.activeSendId = null;
    this.wakeCreditWaiters(true);
    this.info = null;
    this.status = null;
    this.setState("reconnecting");
    this.scheduleReconnect(1400);
  }
  onLine(line: string) {
    let f;
    try {
      f = decode(line);
    } catch (e) {
      // Raw text is JSON-quoted so stray bytes/CR are visible (e.g. a firmware "!wdg" line).
      this.dbg(
        "RX?",
        `${JSON.stringify(line)} (${e instanceof Error ? e.message : String(e)})`,
      );
      this.emit("log", {
        level: "warn",
        message: `Ignored non-TBP UART line: ${JSON.stringify(line)}`,
      });
      return;
    }
    this.dbg(
      "RX",
      `id=${f.id} op=${f.op.toString(16).padStart(2, "0")} ${line.trim()}`,
    );
    if (f.op === KIND.EVENT) {
      this.emit("event", { id: f.id, data: [...f.data] });
      if (f.data[0] === 1 && f.data.length >= 14)
        this.status = this.parseStatusBody(f.data.slice(1));
      this.emit("printer", { info: this.info, status: this.status });
      return;
    }
    const p = this.pending.get(f.id);
    if (!p) {
      this.emit("frame", {
        direction: "rx",
        id: f.id,
        op: f.op,
        data: [...f.data],
      });
      return;
    }
    if (f.op === KIND.ACK) {
      if (!p.acked) p.retries = 0;
      p.acked = true;
      this.releaseSend(f.id);
      this.emit("ack", { id: f.id, opcode: p.op, queueUsed: f.data[0] ?? 0 });
      if (!p.waitTerminal) {
        if (p.timer) clearTimeout(p.timer);
        this.pending.delete(f.id);
        this.wakeCreditWaiters();
        p.resolve({ id: f.id, op: KIND.ACK, data: f.data });
      } else {
        if (p.timer) clearTimeout(p.timer);
        p.timer = setTimeout(() => p.retry?.(), p.timeout);
      }
      return;
    }
    if (f.op === KIND.ERR) {
      const code = f.data[0] ?? 0,
        arg = f.data[1] ?? 0;
      if (p.timer) clearTimeout(p.timer);
      // BUSY and QFULL mean the command was not accepted; retain its ID and
      // retry after the queue has had time to advance, without closing UART.
      if (!p.acked && (code === 5 || code === 9)) {
        // Keep the sender lock: later command IDs must not overtake a rejected ID.
        p.retries = Math.max(0, p.retries - 1);
        p.timer = setTimeout(() => p.retry?.(), 100);
        this.emit("command-retry", {
          id: f.id,
          code,
          message: "Printer busy; retrying",
        });
        return;
      }
      if (p.acked) {
        const error = new Error(
          "TBP ERR after ACK; execution status is ambiguous",
        );
        this.pending.delete(f.id);
        this.wakeCreditWaiters();
        p.reject(error);
        this.emit("log", { level: "warn", message: error.message });
        return;
      }
      // ERR SEQ is an explicit rejection: this frame was not accepted. Retry
      // the same payload at the printer's expected ID, but only when no other
      // transaction could be invalidated by that resynchronization.
      if (code === 6 && arg >= 1 && arg <= 255 && p.retry) {
        if (arg !== f.id && this.pending.size > 1) {
          const error = new Error(
            "TBP sequence mismatch with concurrent commands; resynchronization required",
          );
          this.pending.delete(f.id);
          this.wakeCreditWaiters();
          p.reject(error);
          this.emit("log", { level: "warn", message: error.message });
          return;
        }
        this.releaseSend(f.id, false);
        p.acked = false;
        p.retry(arg);
        return;
      }
      this.releaseSend(f.id, false);
      if (code !== 6) this.nextId = f.id || this.nextId;
      const hasWaitingForCredit = this.creditWaiters.length > 0;
      const error = new Error(
        `TBP ERR 0x${code.toString(16).padStart(2, "0")} (arg ${arg})`,
      );
      this.pending.delete(f.id);
      this.wakeCreditWaiters();
      p.reject(error);
      this.emit("command-error", { id: f.id, message: error.message });
      // Later IDs stay local until the preceding frame is ACKed. If it is
      // rejected, discard unsent work so the rejected ID can be retried safely.
      for (const queued of this.outbound.splice(0)) {
        const later = this.pending.get(queued.id);
        if (!later) continue;
        if (later.timer) clearTimeout(later.timer);
        this.pending.delete(queued.id);
        later.reject(new Error("Blocked by preceding TBP command rejection"));
      }
      this.wakeCreditWaiters(true);
      if (hasWaitingForCredit) {
        for (const resolve of this.creditWaiters.splice(0)) resolve();
      }
      this.dispatchSend();
      return;
    }
    if (f.op === KIND.STATUS) {
      const state = f.data[0];
      p.acked = true; // STATUS also proves acceptance if the ACK was lost.
      this.releaseSend(f.id);
      p.retries = 0;
      this.emit("job-status", { id: f.id, state, body: [...f.data.slice(1)] });
      if (
        state === STATE.DONE ||
        state === STATE.FAILED ||
        state === STATE.CANCELLED
      ) {
        if (p.timer) clearTimeout(p.timer);
        this.pending.delete(f.id);
        this.wakeCreditWaiters();
        this.releaseSend(f.id);
        if (state === STATE.FAILED) {
          const error = new Error(
            `Printer job ${f.id} failed: ${f.data[1] ?? 0}`,
          );
          p.reject(error);
          this.emit("toast", { level: "error", message: error.message });
        } else {
          p.resolve(f);
        }
      } else {
        if (p.timer) clearTimeout(p.timer);
        p.timer = setTimeout(() => p.retry?.(), p.timeout);
      }
    }
  }
  parseStatusBody(b: number[]) {
    const u = (i: number) => (b[i] ?? 255) * 256 + (b[i + 1] ?? 255);
    const headTempRaw = u(5);
    return {
      state: b[0],
      queueUsed: b[1],
      queueDepth: b[2],
      runningId: b[3],
      lastTerminalId: b[4],
      headTempRaw,
      headTempC: headTempCFromRaw(headTempRaw),
      batteryRaw: u(7),
      batteryMv: u(9),
      paper: b[11],
      flags: b[12],
    };
  }
  transact(
    op: number,
    data = new Uint8Array(),
    waitTerminal = true,
    timeout = 1200,
  ): Promise<any> {
    return this.transactNow(op, data, waitTerminal, timeout);
  }
  private async transactNow(
    op: number,
    data = new Uint8Array(),
    waitTerminal = true,
    timeout = 1200,
  ) {
    if (this.mock) {
      const id = op === OP.HELLO ? 1 : this.nextId;
      if (op !== OP.HELLO && op !== OP.ESTOP)
        this.nextId = id >= 255 ? 1 : id + 1;
      if (op === OP.STAT) {
        const body = Uint8Array.of(3, 0, 4, 0, 0, 1, 54, 2, 0, 14, 216, 1, 0);
        return {
          id,
          op: KIND.STATUS,
          data: Uint8Array.of(STATE.DONE, ...body),
        };
      }
      return { id, op: KIND.STATUS, data: Uint8Array.of(STATE.DONE) };
    }
    if (!this.serial?.isOpen || (this.state !== "connected" && op !== OP.HELLO))
      throw new Error("UART is not connected");
    await this.waitForCredit(op);
    this.creditReservations--;
    if (
      !this.serial?.isOpen ||
      (this.state !== "connected" && op !== OP.HELLO)
    ) {
      this.wakeCreditWaiters();
      throw new Error("UART is not connected");
    }
    let id = this.nextId;
    if (op === OP.HELLO) id = 1;
    else {
      id = this.nextId;
      if (id === 0) id = 1;
    }
    let wire = encode(id, op, data);
    const promise = new Promise<any>((resolve, reject) => {
      const p: Pending = {
        id,
        op,
        resolve,
        reject,
        acked: false,
        waitTerminal,
        retries: 0,
        timeout,
      };
      this.pending.set(id, p);
      const attempt = (expectedId?: number) => {
        if (expectedId !== undefined && expectedId !== id) {
          this.pending.delete(id);
          id = expectedId;
          p.id = id;
          wire = encode(id, op, data);
          this.pending.set(id, p);
        }
        if (op !== OP.ESTOP) this.nextId = id >= 255 ? 1 : id + 1;
        if (!this.pending.has(id)) return;
        if (p.retries >= 3) {
          this.pending.delete(id);
          const error = new Error("TBP ACK timeout; link retained");
          reject(error);
          this.wakeCreditWaiters();
          this.releaseSend(id);
          this.emit("log", { level: "warn", message: error.message });
          return;
        }
        const sendFrame = () => {
          p.retries++;
          this.dbg(
            "TX",
            `id=${id} op=${op.toString(16).padStart(2, "0")} ${wire.trim()}${p.retries > 1 ? ` (attempt ${p.retries})` : ""}`,
          );
          this.serial?.write(wire, (e) => {
            if (e) {
              this.dbg("ERR", "UART write: " + e.message);
              this.drop(e.message);
              return;
            }
          });
          if (p.timer) clearTimeout(p.timer);
          if (this.pending.has(id)) {
            p.timer = setTimeout(() => {
              if (p.acked && !waitTerminal) {
                this.pending.delete(id);
                this.wakeCreditWaiters();
                this.releaseSend(id);
                resolve({ id, op, data: new Uint8Array() });
              } else attempt();
            }, timeout);
          }
        };
        if (p.acked || this.activeSendId === id) sendFrame();
        else this.queueSend(id, sendFrame, expectedId !== undefined);
      };
      p.retry = attempt;
      attempt();
    });
    const frame = await promise;
    return frame;
  }
  private requireHeatReady(burnUs: number) {
    if (burnUs <= 0 || this.mock) return;
    const i = this.info;
    const requiredSensors = 0x01 | 0x02 | 0x08;
    if (
      !i ||
      i.burnMaxUs <= 0 ||
      burnUs > i.burnMaxUs ||
      i.burnTickUs <= 0 ||
      i.guardUs <= 0 ||
      i.dutyWindowMs <= 0 ||
      i.dutyMaxUs <= 0 ||
      i.shiftUs <= 0 ||
      i.tempLimitRaw === 0xffff ||
      (i.battMinRaw === 0xffff && !(i.caps & 0x04)) ||
      (i.caps & requiredSensors) !== requiredSensors
    ) {
      throw new Error(
        "Heating disabled: the firmware does not report valid temperature, battery and paper readings or heater/duty limits right now (check the printer has paper, battery and a connected head sensor); use burnUs=0 for heatless feed tests.",
      );
    }
  }
  async command(action: string, msg: any) {
    switch (action) {
      case "ports":
        return { ports: await this.ports() };
      case "connect": {
        const requested = String(msg.path ?? "");
        this.path = requested || autoPath;
        await this.connect(this.path);
        return { state: this.state };
      }
      case "disconnect":
        this.connectWanted = false;
        this.stopKeepalive();
        if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
        this.reconnectTimer = null;
        this.closePort();
        this.setState("disconnected");
        return {};
      case "status": {
        const f = await this.transact(OP.STAT, new Uint8Array(), true);
        if (f.op === KIND.STATUS && f.data.length >= 14)
          this.status = this.parseStatusBody([...f.data.slice(1)]);
        this.emit("printer", { info: this.info, status: this.status });
        return { status: this.status };
      }
      case "flush":
        await this.transact(OP.FLUSH, new Uint8Array(), true);
        return {};
      case "estop": {
        this.stopRequested = true;
        await this.transact(OP.ESTOP, new Uint8Array(), true, 60000);
        return { stopping: true };
      }
      case "move": {
        // The UI sends distanceMm / speedMmS; raw steps / periodUs still work for scripts.
        const { steps: n, periodUs: period } = motionFromMessage(msg, {
          periodUs: 1000,
        });
        const timeout = Math.min(
          60000,
          Math.ceil((Math.abs(n) * period) / 1000) + 2000,
        );
        await this.transact(OP.MOVE, move(n, period), true, timeout);
        if (msg.releaseAfter)
          await this.transact(OP.COILS, Uint8Array.of(0), true);
        return { done: true };
      }
      case "dwell": {
        const ms = Number(msg.ms);
        await this.transact(OP.DWELL, u16(ms), true, ms + 2000);
        return { done: true };
      }
      case "row": {
        const mask = Uint8Array.from({ length: 48 }, (_, i) =>
          msg.black === false ? 0 : 255,
        );
        const burn = Number(msg.burnUs ?? 1500);
        const { steps, periodUs: period } = motionFromMessage(msg, {
          steps: 8,
          periodUs: 6000,
        });
        this.requireHeatReady(burn);
        const timeout = Math.min(
          30000,
          Math.ceil((Math.abs(steps) * period) / 1000) + 5000,
        );
        await this.transact(
          OP.ROW,
          row(mask, burn, steps, period, Number(msg.slices ?? 1)),
          true,
          timeout,
        );
        return { done: true };
      }
      case "print-job": {
        const rows = Array.isArray(msg.rows) ? msg.rows : [];
        if (!rows.length || rows.length > 2000)
          throw new Error("print-job needs 1..2000 rows");
        this.stopRequested = false;
        let sent = 0;
        const depth = Math.max(
          1,
          Math.min(4, Number(this.info?.queueDepth ?? 4)),
        );
        for (let base = 0; base < rows.length; base += depth) {
          if (this.stopRequested) break;
          const batch = rows.slice(base, base + depth);
          await Promise.all(
            batch.map(async (item: any) => {
              if (this.stopRequested) return;
              const mask = Uint8Array.from({ length: 48 }, (_, i) => {
                const hex = String(item.maskHex ?? "");
                if (!/^[0-9a-f]{96}$/i.test(hex))
                  throw new Error(
                    "Each print-job mask must be 96 hex characters",
                  );
                return Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
              });
              const burn = Number(item.burnUs ?? 0);
              const { steps, periodUs: period } = motionFromMessage(item, {
                steps: 0,
                periodUs: 0,
              });
              this.requireHeatReady(burn);
              const timeout = Math.min(
                30000,
                Math.ceil((Math.abs(steps) * period) / 1000) + 5000,
              );
              await this.transact(
                OP.ROW,
                row(mask, burn, steps, period, Number(item.slices ?? 1)),
                true,
                timeout,
              );
              sent++;
              this.emit("print-progress", { done: sent, total: rows.length });
            }),
          );
        }
        return { done: sent, total: rows.length, stopped: this.stopRequested };
      }
      case "print-solid": {
        const rows = Math.max(1, Math.min(500, Number(msg.rows ?? 24)));
        const burn = Number(msg.burnUs ?? 1500);
        const { steps, periodUs: period } = motionFromMessage(msg, {
          steps: 8,
          periodUs: 6000,
        });
        this.requireHeatReady(burn);
        this.stopRequested = false;
        let sent = 0;
        for (let i = 0; i < rows; i++) {
          if (this.stopRequested) break;
          const timeout = Math.min(
            30000,
            Math.ceil((Math.abs(steps) * period) / 1000) + 5000,
          );
          await this.transact(
            OP.ROW,
            row(
              new Uint8Array(48).fill(255),
              burn,
              steps,
              period,
              Number(msg.slices ?? 1),
            ),
            true,
            timeout,
          );
          sent = i + 1;
          this.emit("print-progress", { done: sent, total: rows });
        }
        if (msg.releaseAfter)
          await this.transact(OP.COILS, Uint8Array.of(0), true);
        return { queued: true, rows: sent, stopped: this.stopRequested };
      }
      case "coils":
        if (msg.enabled && !this.mock) {
          // The firmware only holds coils for a bounded time (CONFIG idle_hold_ms, max 10 s) and then releases them.
          const holdMs = Math.max(
            100,
            Math.min(10000, Number(msg.holdMs ?? 5000)),
          );
          await this.transact(
            OP.CONFIG,
            join(Uint8Array.of(2), u16(holdMs)),
            true,
          );
        }
        await this.transact(OP.COILS, Uint8Array.of(msg.enabled ? 1 : 0), true);
        return {};
      case "config":
        await this.transact(
          OP.CONFIG,
          join(Uint8Array.of(Number(msg.key)), u16(Number(msg.value))),
          true,
        );
        return {};
      default:
        throw new Error("Unknown action: " + action);
    }
  }
  async shutdown() {
    this.stopped = true;
    this.connectWanted = false;
    this.stopKeepalive();
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.closePort();
    for (const p of this.pending.values()) {
      if (p.timer) clearTimeout(p.timer);
      p.reject(new Error("Server stopping"));
    }
    this.pending.clear();
  }
}
if (import.meta.main) {
  const link = new PrinterLink();
  const mime: Record<string, string> = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".svg": "image/svg+xml",
  };
  const server = Bun.serve<{ client?: Client }>({
    hostname: host,
    port,
    async fetch(req, server) {
      const url = new URL(req.url);
      if (url.pathname === "/ws") {
        if (server.upgrade(req, { data: {} })) return;
        return new Response("WebSocket upgrade failed", { status: 400 });
      }
      if (url.pathname === "/api/status")
        return Response.json({
          state: link.state,
          path: link.path,
          info: link.info,
          status: link.status,
          error: link.lastError,
        });
      if (url.pathname === "/api/ports")
        return Response.json(await link.ports());
      const rel = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
      const path = resolve(root, rel);
      if (!path.startsWith(root + "/") && !path.startsWith(root))
        return new Response("Not found", { status: 404 });
      if (!existsSync(path)) return new Response("Not found", { status: 404 });
      return new Response(readFileSync(path), {
        headers: {
          "content-type": mime[extname(path)] ?? "application/octet-stream",
          "cache-control": "no-cache",
        },
      });
    },
    websocket: {
      open(ws) {
        const client = { send: (s: string) => ws.send(s) };
        ws.data.client = client;
        link.clients.add(client);
        ws.send(
          JSON.stringify({
            type: "snapshot",
            state: link.state,
            path: link.path,
            info: link.info,
            status: link.status,
            error: link.lastError,
          }),
        );
      },
      message(ws, message) {
        let msg: any;
        try {
          msg = JSON.parse(String(message));
        } catch {
          return;
        }
        if (msg.action === "connect") link.connectWanted = true;
        void link
          .command(String(msg.action ?? ""), msg)
          .then((result) =>
            ws.send(
              JSON.stringify({
                type: "result",
                requestId: msg.requestId,
                action: msg.action,
                ...result,
              }),
            ),
          )
          .catch((e) =>
            ws.send(
              JSON.stringify({
                type: "result",
                requestId: msg.requestId,
                action: msg.action,
                error: e instanceof Error ? e.message : String(e),
              }),
            ),
          );
      },
      close(ws) {
        if (ws.data.client) link.clients.delete(ws.data.client);
      },
    },
  });
  await link.start();
  console.log(`Pinbridge host listening on http://${host}:${server.port}`);
  if (debugFrames) console.log("Debug: logging raw UART frames to stdout");
  process.on("SIGINT", () => {
    void link.shutdown().finally(() => process.exit(0));
  });
  process.on("SIGTERM", () => {
    void link.shutdown().finally(() => process.exit(0));
  });
}
