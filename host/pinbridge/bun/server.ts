import { existsSync, readFileSync } from "node:fs";
import { resolve, extname } from "node:path";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";
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
};
class PrinterLink {
  serial: SerialPort | null = null;
  parser: ReadlineParser | null = null;
  path = "";
  state: "disconnected" | "connecting" | "connected" | "reconnecting" =
    "disconnected";
  info: any = null;
  status: any = null;
  lastError = "";
  nextId = 1;
  pending = new Map<number, Pending>();
  clients = new Set<Client>();
  reconnectTimer: any = null;
  keepaliveTimer: any = null;
  stopped = false;
  generation = 0;
  connectWanted = true;
  stopRequested = false;
  mock = mockMode;
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
      if (this.state !== "connected" || this.pending.size) return;
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
      return (await SerialPort.list()).map((p) => ({
        path: p.path,
        manufacturer: p.manufacturer ?? "",
        serialNumber: p.serialNumber ?? "",
        vendorId: p.vendorId ?? "",
        productId: p.productId ?? "",
      }));
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
        queueDepth: 1,
        maxWindow: 1,
        minPeriodUs: 1000,
        stepTickUs: 1000,
        burnTickUs: 1,
        burnMaxUs: 6000,
        guardUs: 100,
        dutyWindowMs: 0,
        dutyMaxUs: 0,
        shiftUs: 3000,
        tempLimitRaw: 65535,
        battMinRaw: 65535,
        caps: 0,
        uid: "MOCK00000001",
      };
      this.status = {
        state: 0,
        queueUsed: 0,
        queueDepth: 1,
        runningId: 0,
        lastTerminalId: 0,
        headTempRaw: 310,
        batteryRaw: 512,
        batteryMv: 3800,
        paper: 1,
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
        this.path =
          (autoPath && list.some((p) => p.path === autoPath)
            ? autoPath
            : list[0]?.path) ?? "";
      if (!this.path) throw new Error("No UART device found");
      const serial = new SerialPort({
        path: this.path,
        baudRate: 115200,
        autoOpen: false,
        lock: true,
      });
      this.serial = serial;
      await new Promise<void>((resolve, reject) =>
        serial.open((e) => (e ? reject(e) : resolve())),
      );
      if (gen !== this.generation) {
        serial.close();
        return;
      }
      const parser = serial.pipe(new ReadlineParser({ delimiter: "\n" }));
      this.parser = parser;
      parser.on("data", (line: string) => this.onLine(String(line)));
      serial.on("error", (e) => this.onPortError(e));
      serial.on("close", () => this.onPortClose());
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
    this.parser = null;
    if (s?.isOpen)
      try {
        s.close();
      } catch {}
  }
  onPortError(e: Error) {
    this.lastError = e.message;
    this.emit("log", { level: "error", message: "UART: " + e.message });
    this.drop("UART error");
  }
  onPortClose() {
    if (this.serial) {
      this.serial = null;
      this.parser = null;
      this.drop("UART closed");
    }
  }
  drop(reason: string) {
    this.stopKeepalive();
    this.generation++;
    for (const p of this.pending.values()) {
      if (p.timer) clearTimeout(p.timer);
      p.reject(new Error(reason));
    }
    this.pending.clear();
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
      this.emit("log", { level: "warn", message: "Ignored non-TBP UART line" });
      return;
    }
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
      p.acked = true;
      this.emit("ack", { id: f.id, opcode: p.op, queueUsed: f.data[0] ?? 0 });
      if (!p.waitTerminal) {
        if (p.timer) clearTimeout(p.timer);
        p.resolve({ id: f.id, op: KIND.ACK, data: f.data });
      }
      return;
    }
    if (f.op === KIND.ERR) {
      const code = f.data[0] ?? 0,
        arg = f.data[1] ?? 0;
      if (code === 6) this.nextId = arg || 1;
      else this.nextId = f.id || this.nextId;
      const error = new Error(
        `TBP ERR 0x${code.toString(16).padStart(2, "0")} (arg ${arg})`,
      );
      if (p.timer) clearTimeout(p.timer);
      this.pending.delete(f.id);
      p.reject(error);
      this.emit("command-error", { id: f.id, message: error.message });
      return;
    }
    if (f.op === KIND.STATUS) {
      const state = f.data[0];
      this.emit("job-status", { id: f.id, state, body: [...f.data.slice(1)] });
      if (
        state === STATE.DONE ||
        state === STATE.FAILED ||
        state === STATE.CANCELLED
      ) {
        if (p.timer) clearTimeout(p.timer);
        this.pending.delete(f.id);
        if (state === STATE.FAILED) {
          const error = new Error(
            `Printer job ${f.id} failed: ${f.data[1] ?? 0}`,
          );
          p.reject(error);
          this.emit("toast", { level: "error", message: error.message });
        } else {
          p.resolve(f);
        }
      }
    }
  }
  parseStatusBody(b: number[]) {
    const u = (i: number) => (b[i] ?? 255) * 256 + (b[i + 1] ?? 255);
    return {
      state: b[0],
      queueUsed: b[1],
      queueDepth: b[2],
      runningId: b[3],
      lastTerminalId: b[4],
      headTempRaw: u(5),
      batteryRaw: u(7),
      batteryMv: u(9),
      paper: b[11],
      flags: b[12],
    };
  }
  async transact(
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
        const body = Uint8Array.of(3, 0, 1, 0, 0, 1, 54, 2, 0, 14, 216, 1, 0);
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
    let id = this.nextId;
    if (op === OP.HELLO) id = 1;
    else {
      id = this.nextId;
      if (id === 0) id = 1;
    }
    const wire = encode(id, op, data);
    const promise = new Promise<any>((resolve, reject) => {
      const p: Pending = {
        id,
        op,
        resolve,
        reject,
        acked: false,
        waitTerminal,
        retries: 0,
      };
      this.pending.set(id, p);
      const attempt = () => {
        if (!this.pending.has(id)) return;
        if (p.retries >= 3) {
          this.pending.delete(id);
          reject(new Error("TBP ACK timeout; reconnecting"));
          this.drop("TBP timeout");
          return;
        }
        p.retries++;
        this.serial?.write(wire, (e) => {
          if (e) {
            this.drop(e.message);
            return;
          }
        });
        p.timer = setTimeout(() => {
          if (p.acked) {
            if (!waitTerminal) {
              this.pending.delete(id);
              resolve({ id, op, data: new Uint8Array() });
            } else {
              p.retries = 3;
              attempt();
            }
          } else attempt();
        }, timeout);
      };
      attempt();
    });
    if (op === OP.HELLO) this.nextId = 2;
    else if (op !== OP.ESTOP) this.nextId = id >= 255 ? 1 : id + 1;
    const frame = await promise;
    return frame;
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
        const n = Number(msg.steps),
          period = Number(msg.periodUs);
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
        const burn = Number(msg.burnUs ?? 1500),
          steps = Number(msg.steps ?? 8),
          period = Number(msg.periodUs ?? 6000);
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
      case "print-solid": {
        const rows = Math.max(1, Math.min(500, Number(msg.rows ?? 24)));
        const burn = Number(msg.burnUs ?? 1500);
        const period = Number(msg.periodUs ?? 6000);
        this.stopRequested = false;
        let sent = 0;
        for (let i = 0; i < rows; i++) {
          if (this.stopRequested) break;
          const timeout = Math.min(
            30000,
            Math.ceil((Math.abs(Number(msg.steps ?? 8)) * period) / 1000) +
              5000,
          );
          await this.transact(
            OP.ROW,
            row(
              new Uint8Array(48).fill(255),
              burn,
              Number(msg.steps ?? 8),
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
    if (url.pathname === "/api/ports") return Response.json(await link.ports());
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
process.on("SIGINT", () => {
  void link.shutdown().finally(() => process.exit(0));
});
process.on("SIGTERM", () => {
  void link.shutdown().finally(() => process.exit(0));
});
