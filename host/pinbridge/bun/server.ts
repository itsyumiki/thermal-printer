import { existsSync, readFileSync } from "node:fs";
import { resolve, extname } from "node:path";
import { Uart, listPorts } from "./uart.ts";
import { motionFromMessage } from "./units.ts";
import { startConsole } from "./console.ts";
import { headTempCFromRaw } from "./thermistor.ts";
import {
  OP,
  KIND,
  STATE,
  encode,
  decode,
  decodeInfo,
  decodeStat,
  ROW_FLAG,
  nameOf,
  type Frame,
  type Message,
  decodeMessage,
  describeMessage,
  decodeTimeTestResult,
  describeTimeTestSummary,
  summarizeTimeTest,
  summarizeTimeTestRuns,
  type TimeTestRun,
  TIME_SRC,
  TIME_FAIL,
  TIME_MS_MIN,
  TIME_MS_MAX,
  translate,
  row,
  move,
  u16,
  join,
  DEFAULT_HEATER_SLICES,
  type Command,
} from "./tbp/index.ts";

const root = resolve(import.meta.dir, "public");
const host = process.env.PINBRIDGE_HOST ?? "127.0.0.1";
const port = Number(process.env.PINBRIDGE_PORT ?? 8765);
const autoPath = process.env.PINBRIDGE_UART ?? "";
const mockMode = process.env.PINBRIDGE_MOCK === "1";
// stdin console: on by default; PINBRIDGE_CONSOLE=0 or --no-console turns it off (e.g. when stdin belongs to something else).
const consoleEnabled =
  process.env.PINBRIDGE_CONSOLE !== "0" &&
  !process.argv.includes("--no-console");
const HELLO_TIMEOUT_MS = 400;
const HELLO_RETRY_DELAY_MS = 100;
// Raw UART frame logging to stdout: PINBRIDGE_DEBUG=1 or `bun run server.ts --debug`.
const debugFrames =
  /^(1|true|yes|on)$/i.test(process.env.PINBRIDGE_DEBUG ?? "") ||
  process.argv.includes("--debug");
function debugStamp() {
  const d = new Date();
  const p = (n: number, w = 2) => String(n).padStart(w, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}
const errText = (e: unknown) => (e instanceof Error ? e.message : String(e));
/**
 * Calls onStall(ms) whenever the JS event loop was blocked for at least thresholdMs. While it is blocked the
 * HTTP server cannot accept a WebSocket upgrade, so this is what a "web UI waits to connect" looks like
 * from the inside. Returns a function that stops the watcher.
 */
export function watchEventLoop(
  onStall: (ms: number) => void,
  intervalMs = 100,
  thresholdMs = 150,
) {
  let last = performance.now();
  const timer = setInterval(() => {
    const now = performance.now();
    const late = now - last - intervalMs;
    last = now;
    if (late >= thresholdMs) onStall(Math.round(late));
  }, intervalMs);
  return () => clearInterval(timer);
}
const pick = (o: any, keys: string[]) =>
  keys
    .filter((k) => o?.[k] !== undefined)
    .map((k) => `${k}=${o[k]}`)
    .join(" ");
/** Short label for the log line of a UI command. */
function describeCommand(action: string, msg: any): string {
  switch (action) {
    case "print-job":
      return `print-job rows=${Array.isArray(msg.rows) ? msg.rows.length : 0}`;
    case "print-solid":
      return `print-solid rows=${msg.rows ?? 24} burnUs=${msg.burnUs ?? 0}`;
    case "move":
      return `move ${pick(msg, ["distanceMm", "speedMmS", "steps", "periodUs", "releaseAfter"])}`.trim();
    case "row":
      return `row ${pick(msg, ["black", "burnUs", "distanceMm", "speedMmS", "steps", "periodUs", "slices"])}`.trim();
    case "dwell":
      return `dwell ${msg.ms}ms`;
    case "coils":
      return `coils ${msg.enabled ? "on" : "off"}`;
    case "config":
      return `config key=${msg.key} value=${msg.value}`;
    case "connect":
      return `connect ${msg.path || "auto"}`;
    default:
      return action;
  }
}
/** What a UI command returned, for its log line (empty when there is nothing worth saying). */
function summarizeResult(action: string, r: any): string {
  switch (action) {
    case "print-job":
    case "print-solid":
      return ` -> ${r.done}/${r.total} rows${r.stopped ? " (stopped)" : ""}`;
    case "connect":
      return ` -> ${r.state}`;
    case "time-test":
      return ` -> ${r.src} x${r.runs}: ratio ${Number(r.ratio).toFixed(4)}${r.withinGate ? "" : " OUT OF GATE"}`;
    case "status": {
      const s = r.status;
      if (!s) return "";
      return ` -> state=${s.state} queue=${s.queueUsed}/${s.queueDepth} head=${s.headTempC ?? "?"}C batt=${s.batteryMv}mV paper=${s.paper} flags=0x${Number(s.flags ?? 0).toString(16)}`;
    }
    default:
      return "";
  }
}
/** Commands that run for a while: their start is logged too, not just their end. */
const LONG_ACTIONS = new Set(["print-job", "print-solid", "time-test"]);
const QUIET_ACTIONS = new Set(["ports", "status"]);
/** Per-command ack/terminal timeout for console and UI sends. */
function commandTimeoutMs(cmd: Command): number {
  switch (cmd.type) {
    case "move":
      return Math.min(
        60000,
        Math.ceil((Math.abs(cmd.steps) * cmd.periodUs) / 1000) + 2000,
      );
    case "row":
      return Math.min(
        30000,
        Math.ceil((Math.abs(cmd.steps) * cmd.periodUs) / 1000) + 5000,
      );
    case "dwell":
      return cmd.ms + 2000;
    case "estop":
      return 5000;
    case "timetest":
      return cmd.ms + 3000; // the printer blocks for the whole run, plus output drain and the reply
    default:
      return 1200;
  }
}
/** How long a raw hex send waits for the printer's reply before giving up. */
const RAW_REPLY_WAIT_MS = 1500;
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
  /** Sees every ACK/STATUS/ERR frame for this id with the host clock at receipt (TIMETEST uses it to time RUNNING..DONE). */
  onFrame?: (f: Frame, at: number) => void;
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
  /** Where the always-on operational log goes (stdout). Raw frames stay behind the debug flag and debugSink. */
  logSink: (line: string) => void = (line) => console.log(line);
  /** What the host is doing right now; the event-loop stall watcher prints it so a stall can be attributed. */
  phase = "idle";
  private throttled = new Map<string, { at: number; hidden: number }>();
  /** One stamped line of the normal-mode log; never throws. */
  logLine(tag: string, text: string) {
    try {
      this.logSink(`${debugStamp()} ${tag.padEnd(5)} ${text}`);
    } catch {}
  }
  log(level: "info" | "warn" | "error", text: string) {
    this.logLine(level.toUpperCase(), text);
  }
  /** log() for a condition that can repeat every second: print it at most once per everyMs, with a count of what was held back. */
  logThrottled(
    key: string,
    level: "info" | "warn" | "error",
    text: string,
    everyMs = 10000,
  ) {
    const now = Date.now();
    const t = this.throttled.get(key);
    if (t && now - t.at < everyMs) {
      t.hidden++;
      return;
    }
    this.throttled.set(key, { at: now, hidden: 0 });
    this.log(
      level,
      t?.hidden ? `${text} (+${t.hidden} more since the last report)` : text,
    );
  }
  /** Log to stdout and to the browser event log. throttleKey keeps a repeating warning from flooding stdout (the browser still gets every one). */
  uiLog(
    level: "info" | "warn" | "error",
    message: string,
    throttleKey?: string,
  ) {
    if (throttleKey) this.logThrottled(throttleKey, level, message);
    else this.log(level, message);
    this.emit("log", { level, message });
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
      this.log("info", `UART: opening ${this.path} at 115200`);
      const serial = new Uart(this.path, 115200);
      serial.onLine = (line) => this.onLine(line);
      serial.onError = (e) => this.onPortError(e);
      serial.onClose = () => this.onPortClose();
      this.serial = serial;
      // open() runs stty and opens the tty off the event loop; the time is logged so a slow device shows up.
      const openStart = performance.now();
      this.phase = `UART open of ${this.path}`;
      try {
        await serial.open();
      } finally {
        this.phase = "idle";
      }
      const openMs = performance.now() - openStart;
      this.log(
        openMs >= 500 ? "warn" : "info",
        `UART: opened ${this.path} in ${openMs.toFixed(0)} ms`,
      );
      if (gen !== this.generation) {
        serial.close();
        return;
      }
      await new Promise((r) => setTimeout(r, 120));
      let helloInfo: ReturnType<typeof decodeInfo> | null = null;
      let helloFailures = 0;
      const helloStart = performance.now();
      while (gen === this.generation && this.serial?.isOpen) {
        try {
          this.serial.discardPartialLine();
          const hello = await this.transact(
            OP.HELLO,
            Uint8Array.of(1),
            true,
            HELLO_TIMEOUT_MS,
          );
          helloInfo = decodeInfo(hello.data);
          break;
        } catch (e) {
          if (gen !== this.generation || !this.serial?.isOpen) throw e;
          const message = errText(e);
          this.lastError = message;
          helloFailures++;
          this.uiLog(
            "warn",
            `HELLO failed; retrying on open UART: ${message}`,
            "hello-failed",
          );
          await new Promise((r) => setTimeout(r, HELLO_RETRY_DELAY_MS));
        }
      }
      if (!helloInfo) throw new Error("UART closed before HELLO completed");
      this.info = helloInfo;
      this.lastError = "";
      this.stopRequested = false;
      this.setState("connected");
      this.startKeepalive();
      this.log(
        "info",
        `printer connected: ${this.path} · TBP v${this.info.version} · build ` +
          `${Number(this.info.buildId ?? 0)
            .toString(16)
            .padStart(8, "0")} · uid ${this.info.uid} · ` +
          `${this.info.dots} dots · queue ${this.info.queueDepth}/window ${this.info.maxWindow} · ` +
          `HELLO took ${(performance.now() - helloStart).toFixed(0)} ms` +
          (helloFailures ? ` after ${helloFailures} failed attempt(s)` : ""),
      );
      this.emit("toast", {
        level: "success",
        message:
          `Printer connected · TBP v${this.info.version} · build ` +
          Number(this.info.buildId ?? 0)
            .toString(16)
            .padStart(8, "0"),
      });
      this.emit("printer", { info: this.info, status: this.status });
    } catch (e) {
      // A newer connect() or drop() owns the link now; this attempt was superseded, not failed.
      if (gen !== this.generation) return;
      this.lastError = errText(e);
      this.logThrottled(
        "connect-failed:" + this.lastError,
        "warn",
        `UART connect failed: ${this.lastError}; retrying`,
        15000,
      );
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
    this.uiLog("error", "UART: " + e.message);
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
    this.log("warn", `link dropped: ${reason}; reconnecting in 1400 ms`);
    this.sequenceResyncing = false;
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
  /** Monotonic millisecond clock used to timestamp printer frames; replaceable in tests. */
  clock: () => number = () => performance.now();
  onLine(line: string) {
    const rxAt = this.clock(); // stamp before any parsing or logging
    let f;
    try {
      f = decode(line);
    } catch (e) {
      // Raw text is JSON-quoted so stray bytes/CR are visible (e.g. a firmware "!wdg" line).
      this.dbg("RX?", `${JSON.stringify(line)} (${errText(e)})`);
      // A stdin session shows the line itself as the printer's reply; otherwise it is unsolicited (e.g. "!wdg").
      if (this.rawSession) {
        this.rawSession.onLine(line.trim());
        return;
      }
      this.uiLog("warn", `Ignored non-TBP UART line: ${JSON.stringify(line)}`);
      return;
    }
    this.rawSession?.onLine(line.trim());
    this.dbg(
      "RX",
      `id=${f.id} op=${f.op.toString(16).padStart(2, "0")} ${line.trim()}`,
    );
    this.logFrame(f);
    if (f.op === KIND.EVENT) {
      this.emit("event", { id: f.id, data: [...f.data] });
      if (f.data[0] === 1 && f.data.length >= 14)
        this.status = this.parseStatusBody(f.data.slice(1));
      this.emit("printer", { info: this.info, status: this.status });
      return;
    }
    const p = this.pending.get(f.id);
    if (p) p.onFrame?.(f, rxAt);
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
        this.releaseSend(f.id, false);
        if (code === 6 && arg >= 1 && arg <= 255) {
          this.nextId = arg;
          for (const queued of this.outbound.splice(0)) {
            const later = this.pending.get(queued.id);
            if (!later) continue;
            if (later.timer) clearTimeout(later.timer);
            this.pending.delete(queued.id);
            later.reject(
              new Error("Blocked by TBP sequence resynchronization"),
            );
          }
          this.uiLog(
            "warn",
            `Sequence mismatch after ACK; next ID reset to ${arg}; current job status is ambiguous`,
          );
          this.wakeCreditWaiters(true);
        } else {
          this.wakeCreditWaiters();
        }
        p.reject(error);
        this.uiLog("warn", error.message);
        return;
      }
      // ERR SEQ means this frame was not accepted. Cancel later unsent IDs,
      // probe STAT at the firmware's expected ID, then retry the original
      // command at the next ID. The UART remains open throughout recovery.
      if (code === 6 && arg >= 1 && arg <= 255 && p.retry) {
        if (this.sequenceResyncing || p.op === OP.HELLO) {
          this.pending.delete(f.id);
          this.releaseSend(f.id, false);
          this.nextId = arg;
          p.reject(
            new Error(
              `TBP sequence resynchronization failed at expected ID ${arg}`,
            ),
          );
          this.wakeCreditWaiters(true);
          return;
        }
        if (this.pending.size === 1 || p.op === OP.STAT) {
          if (this.pending.size > 1) {
            this.outbound.splice(0);
            for (const [pendingId, later] of this.pending) {
              if (later === p) continue;
              if (later.timer) clearTimeout(later.timer);
              this.pending.delete(pendingId);
              later.reject(
                new Error(
                  "Cancelled unsent command after TBP sequence mismatch",
                ),
              );
            }
          }
          this.releaseSend(f.id, false);
          p.acked = false;
          p.retries = 0;
          p.retry(arg);
          return;
        }
        this.outbound.splice(0);
        for (const [pendingId, later] of this.pending) {
          if (later === p) continue;
          if (later.timer) clearTimeout(later.timer);
          this.pending.delete(pendingId);
          later.reject(
            new Error(
              "Cancelled unsent command after TBP sequence mismatch; retry after resync",
            ),
          );
        }
        this.pending.delete(f.id);
        this.releaseSend(f.id, false);
        p.acked = false;
        p.retries = 0;
        this.nextId = arg;
        this.sequenceResyncing = true;
        this.uiLog(
          "warn",
          `TBP sequence mismatch; probing expected ID ${arg} with STAT before retrying command`,
        );
        void this.transact(OP.STAT, new Uint8Array(), true, 400)
          .then(() => {
            this.sequenceResyncing = false;
            p.retries = 0;
            p.acked = false;
            p.retry?.(this.nextId);
            this.wakeCreditWaiters(true);
          })
          .catch((resyncError) => {
            this.sequenceResyncing = false;
            const reason =
              resyncError instanceof Error
                ? resyncError.message
                : String(resyncError);
            p.reject(
              new Error(`TBP sequence resynchronization failed: ${reason}`),
            );
            this.wakeCreditWaiters(true);
            this.uiLog(
              "warn",
              `TBP sequence resynchronization failed: ${reason}`,
            );
          });
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
  /** Normal-mode log line for the printer messages worth reading: failures, cancellations, events, odd row flags. */
  private logFrame(f: Frame) {
    let m: Message;
    try {
      m = decodeMessage(f, { opOf: (id) => this.pending.get(id)?.op });
    } catch {
      return;
    }
    const opName = (id: number) => {
      const op = this.pending.get(id)?.op;
      const name = op === undefined ? undefined : nameOf(OP, op);
      return name ? ` (${name.toLowerCase()})` : "";
    };
    if (m.type === "event") {
      if (m.event === "heartbeat") return;
      const level = m.event === "paper" ? "info" : "warn";
      this.log(level, `printer: ${describeMessage(m)}`);
      return;
    }
    if (m.type === "err") {
      // The protocol parser has decoded the numeric return code and its name.
      // Keep repeated BUSY/QFULL/SEQ replies from flooding stdout during retries.
      this.logThrottled(
        `tbp-err:${m.code}`,
        "warn",
        `printer: ${describeMessage(m)}${opName(f.id)}`,
        5000,
      );
      return;
    }
    if (m.type !== "status") return;
    const responseOp = this.pending.get(f.id)?.op;
    if (
      m.state === "done" &&
      responseOp !== OP.STAT &&
      responseOp !== OP.HELLO
    ) {
      this.log("info", `printer: ${describeMessage(m)}${opName(f.id)}`);
    }
    if (m.state === "failed" || m.state === "cancelled") {
      const text = describeMessage(m).replace(
        / id=(\d+)/,
        ` id=$1${opName(f.id)}`,
      );
      this.log(m.state === "failed" ? "error" : "warn", `printer: ${text}`);
    } else if (m.state === "done" && m.result.kind === "row") {
      const odd = m.result.row.flags & ~ROW_FLAG.NO_HEAT & ~ROW_FLAG.COLD_START;
      if (odd)
        this.logThrottled(
          `row-flags:${odd}`,
          "warn",
          `printer: ${describeMessage(m)}`,
          5000,
        );
    } else if (
      m.state === "done" &&
      m.result.kind === "flushed" &&
      m.result.flushed > 0
    ) {
      this.log(
        "info",
        `printer: ${m.result.flushed} queued command(s) discarded`,
      );
    }
  }
  parseStatusBody(b: number[]) {
    const stat = decodeStat(b);
    return { ...stat, headTempC: headTempCFromRaw(stat.headTempRaw) };
  }
  transact(
    op: number,
    data = new Uint8Array(),
    waitTerminal = true,
    timeout = 1200,
    onFrame?: (f: Frame, at: number) => void,
  ): Promise<any> {
    return this.transactNow(op, data, waitTerminal, timeout, onFrame);
  }
  private async transactNow(
    op: number,
    data = new Uint8Array(),
    waitTerminal = true,
    timeout = 1200,
    onFrame?: (f: Frame, at: number) => void,
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
      let hasBeenSent = false;
      const p: Pending = {
        id,
        op,
        resolve,
        reject,
        acked: false,
        waitTerminal,
        retries: 0,
        timeout,
        onFrame,
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
        if (op !== OP.ESTOP && (!hasBeenSent || expectedId !== undefined))
          this.nextId = id >= 255 ? 1 : id + 1;
        if (!this.pending.has(id)) return;
        if (p.retries >= 3) {
          this.pending.delete(id);
          const error = new Error("TBP ACK timeout; link retained");
          reject(error);
          this.wakeCreditWaiters();
          this.releaseSend(id);
          this.uiLog("warn", error.message);
          return;
        }
        const sendFrame = () => {
          if (op === OP.HELLO) this.serial?.discardPartialLine?.();
          p.retries++;
          hasBeenSent = true;
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
  private timeTestBusy = false;
  /**
   * One binary TIMETEST (F0) run: the host clock at the printer's RUNNING frame and at its DONE frame bracket the
   * counted interval (the firmware writes both straight to USB), and the DONE body says what it counted.
   */
  private async timeRun(src: string, ms: number): Promise<TimeTestRun> {
    const mark = {
      begin: null as number | null,
      end: null as number | null,
      failArg: null as number | null,
    };
    const wire = Uint8Array.of(
      TIME_SRC[src as keyof typeof TIME_SRC],
      ms >> 8,
      ms & 0xff,
    );
    let frame: any;
    try {
      // The printer blocks for the whole run, so the ACK/status timeout must outlast it or the frame is resent.
      frame = await this.transact(
        OP.TIMETEST,
        wire,
        true,
        ms + 3000,
        (f, at) => {
          if (f.op !== KIND.STATUS) return;
          const state = f.data[0];
          if (state === STATE.RUNNING) mark.begin = at;
          else if (state === STATE.DONE) mark.end = at;
          else if (state === STATE.FAILED) mark.failArg = f.data[2] ?? null;
        },
      );
    } catch (e) {
      const why =
        mark.failArg === TIME_FAIL.TIMEOUT
          ? "the firmware's timer never finished (its compare interrupt did not fire)"
          : mark.failArg === TIME_FAIL.ABORTED
            ? "aborted by an emergency stop or a USB detach"
            : mark.failArg === TIME_FAIL.SETUP
              ? "could not start (printer busy, or that timer is in use)"
              : null;
      throw why ? new Error(`TIMETEST ${src}: ${why}`) : e;
    }
    if (mark.begin === null || mark.end === null)
      throw new Error(`TIMETEST ${src}: no RUNNING/DONE pair to time`);
    return {
      test: decodeTimeTestResult([...frame.data.slice(1)]),
      wallMs: mark.end - mark.begin,
    };
  }
  /**
   * Timer self-test: `reps` runs of one clock source, each counting about `ms` in the source's own units while
   * the host times RUNNING..DONE. The printer cannot answer anything else while counting, so the keepalive is
   * paused for the duration. Nothing heats or moves.
   */
  async timeTest(src: string, ms = 1000, reps = 5) {
    if (!Object.hasOwn(TIME_SRC, src))
      throw new Error(`Unknown timer source: ${src}`);
    ms = Math.max(
      TIME_MS_MIN,
      Math.min(TIME_MS_MAX, Math.trunc(Number(ms) || 1000)),
    );
    reps = Math.max(1, Math.min(10, Math.trunc(Number(reps) || 5)));
    if (this.mock) {
      const runs: TimeTestRun[] = Array.from({ length: reps }, () => ({
        test: {
          src: TIME_SRC[src as keyof typeof TIME_SRC],
          n: ms,
          wantUs: ms * 1000,
          dj: 0,
          ticks: 0,
          div: 0,
          isr: 0,
        },
        wallMs: ms,
      }));
      return { ...summarizeTimeTestRuns(runs), mock: true };
    }
    if (this.timeTestBusy)
      throw new Error("A timer self-test is already running");
    if (!this.serial?.isOpen || this.state !== "connected")
      throw new Error("UART is not connected");
    this.timeTestBusy = true;
    this.stopKeepalive();
    try {
      const runs: TimeTestRun[] = [];
      for (let i = 0; i < reps; i++) {
        const run = await this.timeRun(src, ms);
        runs.push(run);
        this.emit("time-test-progress", {
          src,
          run: i + 1,
          reps,
          wallMs: run.wallMs,
          wantMs: run.test.wantUs / 1000,
        });
      }
      return summarizeTimeTestRuns(runs);
    } finally {
      this.timeTestBusy = false;
      if (this.state === "connected" && !this.rawSession) this.startKeepalive();
    }
  }
  private requireHeatReady(burnUs: number) {
    if (burnUs <= 0 || this.mock) return;
    const i = this.info;
    const requiredSensors = 0x01 | 0x02 | 0x08;
    if (
      !i ||
      i.burnMaxUs <= 0 ||
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
  /** Line callback of an in-flight console send; it sees every line the printer writes, valid TBP or not. */
  rawSession: { onLine: (line: string) => void } | null = null;
  /**
   * The stdin console entry point: one hex frame in, the lines to show out. TBP is hex only, so anything
   * else is refused. The frame goes to the UART exactly as typed (its id and CRC are not touched) and every
   * line the printer writes back is shown until the command's own terminal reply arrives or the wait runs out.
   * If the printer accepted the frame, the host's next command id follows it so the web UI keeps working.
   */
  async sendHex(input: string): Promise<string[]> {
    const hex = input.trim().toUpperCase();
    if (!/^[0-9A-F]+$/.test(hex) || hex.length % 2)
      throw new Error(
        "hex frames only: an even number of hex digits such as 01010101E654 (no words, no spaces)",
      );
    if (!this.serial?.isOpen)
      throw new Error("no open UART to send a frame on");
    if (this.rawSession)
      throw new Error("another frame is still waiting for its reply");
    const sent = translate(hex);
    const out: string[] = [];
    let id: number | null = null;
    let cmd: Command | null = null;
    let waitMs = RAW_REPLY_WAIT_MS;
    if (sent.ok && sent.direction === "to-printer") {
      id = sent.id;
      cmd = sent.command;
      waitMs = Math.max(waitMs, commandTimeoutMs(cmd));
      out.push(`TX ${hex}  [${sent.text}]`);
    } else if (sent.ok) {
      out.push(
        `TX ${hex}  [${sent.text}; printer->host frames are not commands, sent as typed]`,
      );
    } else {
      out.push(`TX ${hex}  (${sent.text}; sent as typed)`);
    }
    const opOf = (frameId: number) =>
      frameId === id ? sent.frame?.op : undefined;
    let accepted = false;
    let seqHint: number | null = null;
    let runStart: number | null = null; // host clock at the printer's RUNNING frame (TIMETEST only)
    this.stopKeepalive();
    try {
      await new Promise<void>((resolve, reject) => {
        const timer = setTimeout(resolve, waitMs);
        const finish = () => {
          clearTimeout(timer);
          resolve();
        };
        this.rawSession = {
          onLine: (line) => {
            const rxAt = performance.now();
            const t = translate(line, { opOf });
            out.push(`RX ${t.ok ? `${t.hex}  [${t.text}]` : line}`);
            if (!t.ok || t.direction !== "from-printer" || id === null) return;
            const m = t.message;
            if (m.id !== id) return;
            if (m.type === "ack") accepted = true;
            if (m.type === "status") {
              accepted = true;
              if (m.state === "running") runStart = rxAt;
              if (m.state === "done") {
                if (m.result.kind === "timetest" && runStart !== null)
                  out.push(
                    describeTimeTestSummary(
                      summarizeTimeTest(m.result.test, rxAt - runStart),
                    ),
                  );
                if (m.result.kind === "info") this.info = m.result.info;
                if (m.result.kind === "stat")
                  this.status = {
                    ...m.result.stat,
                    headTempC: headTempCFromRaw(m.result.stat.headTempRaw),
                  };
              }
            }
            if (m.type === "err" && m.code === 6 && m.arg >= 1) seqHint = m.arg;
            const terminal =
              m.type === "err" ||
              (m.type === "status" &&
                ["done", "failed", "cancelled"].includes(m.state));
            if (terminal) finish();
          },
        };
        this.dbg("TX", `console ${hex}`);
        this.serial!.write(hex + "\n", (e) => {
          if (e) {
            clearTimeout(timer);
            reject(e);
          }
        });
      });
    } finally {
      this.rawSession = null;
      if (this.state === "connected") this.startKeepalive();
    }
    if (cmd && id !== null && cmd.type !== "estop") {
      if (accepted) this.nextId = id >= 255 ? 1 : id + 1;
      else if (seqHint !== null) this.nextId = seqHint;
    }
    if (cmd?.type === "stat")
      this.emit("printer", { info: this.info, status: this.status });
    if (out.length === 1) out.push("(no reply)");
    return out;
  }
  /** UI/console entry that logs what it was asked and what came back (ports/status stay quiet: the UI polls them). */
  async loggedCommand(action: string, msg: any) {
    const label = describeCommand(action, msg);
    const quiet = QUIET_ACTIONS.has(action);
    const started = performance.now();
    const took = () => `${(performance.now() - started).toFixed(0)} ms`;
    if (!quiet && LONG_ACTIONS.has(action))
      this.log("info", `cmd: ${label} started`);
    try {
      const result = await this.command(action, msg);
      if (!quiet)
        this.log(
          "info",
          `cmd: ${label}${summarizeResult(action, result)} (${took()})`,
        );
      return result;
    } catch (e) {
      this.log("warn", `cmd: ${label} failed: ${errText(e)} (${took()})`);
      throw e;
    }
  }
  async command(action: string, msg: any) {
    // The firmware cannot answer anything while a TIMETEST run is counting.
    if (
      this.timeTestBusy &&
      !["ports", "connect", "disconnect", "time-test"].includes(action)
    )
      throw new Error("A timer self-test is running; wait for it to finish");
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
      case "time-test":
        return await this.timeTest(
          String(msg.src ?? ""),
          Number(msg.ms ?? 1000),
          Number(msg.reps ?? 5),
        );
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
        const burn = Number(msg.burnUs ?? 0);
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
          row(
            mask,
            burn,
            steps,
            period,
            Number(
              msg.slices ??
                (burn > 0 && Math.abs(steps) >= DEFAULT_HEATER_SLICES
                  ? DEFAULT_HEATER_SLICES
                  : 1),
            ),
          ),
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
                row(
                  mask,
                  burn,
                  steps,
                  period,
                  Number(
                    item.slices ??
                      (burn > 0 && Math.abs(steps) >= DEFAULT_HEATER_SLICES
                        ? DEFAULT_HEATER_SLICES
                        : 1),
                  ),
                ),
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
        const burn = Number(msg.burnUs ?? 0);
        const stepsPerRow = 4;
        const period = 6000;
        const slices = 4;
        this.requireHeatReady(burn);
        this.stopRequested = false;
        let nextRow = 0;
        let completedRows = 0;
        const workerCount = Math.max(
          1,
          Math.min(3, Number(this.info?.maxWindow ?? 4) - 1),
        );
        // One ROW per logical image row: one shift/latch and four heater slices
        // synchronized to feed edges. Keep a small compatible window in flight so
        // the firmware can extend the continuous feed train across row boundaries.
        const worker = async () => {
          while (!this.stopRequested) {
            const rowIndex = nextRow++;
            if (rowIndex >= rows) return;
            const timeout = Math.min(
              30000,
              Math.ceil((stepsPerRow * period) / 1000) + 10000,
            );
            try {
              await this.transact(
                OP.ROW,
                row(
                  new Uint8Array(48).fill(255),
                  burn,
                  stepsPerRow,
                  period,
                  slices,
                ),
                true,
                timeout,
              );
            } catch (error) {
              this.stopRequested = true;
              throw error;
            }
            completedRows++;
            this.emit("print-progress", { done: completedRows, total: rows });
          }
        };
        await Promise.all(Array.from({ length: workerCount }, () => worker()));
        if (msg.releaseAfter && !this.stopRequested && completedRows === rows)
          await this.transact(OP.COILS, Uint8Array.of(0), true);
        return {
          done: completedRows,
          total: rows,
          stopped: this.stopRequested,
        };
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
        link.log("info", `web UI connected (${link.clients.size} open)`);
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
          .loggedCommand(String(msg.action ?? ""), msg)
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
        link.log("info", `web UI disconnected (${link.clients.size} open)`);
      },
    },
  });
  // A blocked event loop is what a web UI that "waits to connect" looks like from the inside: say so, and say why.
  watchEventLoop((ms) =>
    link.logThrottled(
      "loop-stall",
      "warn",
      `host event loop was blocked for ${ms} ms${link.phase === "idle" ? "" : ` during: ${link.phase}`}; web UI requests waited`,
      5000,
    ),
  );
  await link.start();
  console.log(`Pinbridge host listening on http://${host}:${server.port}`);
  if (debugFrames) console.log("Debug: logging raw UART frames to stdout");
  if (consoleEnabled) {
    startConsole(link, process.stdin);
    if (process.stdin.isTTY)
      console.log(
        "Console: paste a hex frame (e.g. 01010101E654 = HELLO); the printer's reply is shown here.",
      );
  }
  let shuttingDown = false;
  const shutdown = () => {
    if (shuttingDown) return;
    shuttingDown = true;
    // Stop accepting requests and close live WebSockets before waiting on UART cleanup.
    server.stop(true);
    void link.shutdown().finally(() => process.exit(0));
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}
