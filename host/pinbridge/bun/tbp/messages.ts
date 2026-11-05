// Printer -> host: frames -> typed messages. Pure functions, no I/O.
import {
  CANCEL_REASON,
  ERR_CODE,
  EVENT,
  KIND,
  OP,
  STATE,
  nameOf,
} from "./protocol.ts";
import { decode, type Frame } from "./frame.ts";

export type Info = ReturnType<typeof decodeInfo>;
export type Stat = ReturnType<typeof decodeStat>;

/** HELLO STATUS DONE. `data` is the whole STATUS DATA: the DONE state byte plus the 38-byte INFO body. */
export function decodeInfo(data: Uint8Array) {
  if (data.length !== 39 || data[0] !== STATE.DONE)
    throw new Error(`Invalid HELLO STATUS payload (${data.length} bytes)`);
  const b = data.subarray(1);
  const n = (i: number) => b[i]! * 256 + b[i + 1]!;
  return {
    version: b[0]!,
    buildId: (b[1]! * 0x1000000 + (b[2]! << 16) + (b[3]! << 8) + b[4]!) >>> 0,
    dots: n(5),
    queueDepth: b[7]!,
    maxWindow: b[8]!,
    minPeriodUs: n(9),
    stepTickUs: n(11),
    burnTickUs: n(13),
    burnMaxUs: n(15),
    guardUs: n(17),
    dutyWindowMs: n(19),
    dutyMaxUs: n(21),
    shiftUs: n(23),
    tempLimitRaw: n(25),
    battMinRaw: n(27),
    caps: b[29]!,
    uid: [...b.slice(30, 38)]
      .map((x) => x.toString(16).padStart(2, "0"))
      .join(""),
  };
}

/** The 13-byte STAT body (also a HEARTBEAT's body after its event byte). Raw ADC fields are 0xFFFF when unavailable. */
export function decodeStat(b: Uint8Array | number[]) {
  if (b.length < 13)
    throw new Error(`STAT body needs 13 bytes, got ${b.length}`);
  const u = (i: number) => b[i]! * 256 + b[i + 1]!;
  return {
    state: b[0]!,
    queueUsed: b[1]!,
    queueDepth: b[2]!,
    runningId: b[3]!,
    lastTerminalId: b[4]!,
    headTempRaw: u(5),
    batteryRaw: u(7),
    batteryMv: u(9),
    paper: b[11]!,
    flags: b[12]!,
  };
}

/** ROW DONE result, without the leading state byte (11 bytes). */
export function decodeRowResult(b: Uint8Array | number[]) {
  if (b.length < 11)
    throw new Error(`ROW result needs 11 bytes, got ${b.length}`);
  const u = (i: number) => b[i]! * 256 + b[i + 1]!;
  return {
    headTempRaw: u(0),
    batteryRaw: u(2),
    batteryMv: u(4),
    burnEffUs: u(6),
    periodEffUs: u(8),
    flags: b[10]!,
  };
}

export type DoneResult =
  | { kind: "info"; info: Info }
  | { kind: "stat"; stat: Stat }
  | { kind: "row"; row: ReturnType<typeof decodeRowResult> }
  | { kind: "flushed"; flushed: number }
  | { kind: "move"; periodEffUs: number }
  | { kind: "config"; key: number; value: number }
  | { kind: "none" }
  | { kind: "unknown"; data: number[] };

export type StatusDetail =
  | { state: "queued"; pos: number; queueUsed: number }
  | { state: "running" }
  | { state: "done"; result: DoneResult }
  | { state: "failed"; code: number; codeName: string; arg: number }
  | { state: "cancelled"; reason: number; reasonName: string }
  | { state: "progress"; remaining: number }
  | { state: "unknown"; stateByte: number; data: number[] };

export type EventDetail =
  | { event: "heartbeat"; stat: Stat }
  | { event: "paper"; present: number }
  | { event: "hot"; active: boolean }
  | { event: "reset"; reason: number }
  | { event: "stopped"; reason: number }
  | { event: "unknown"; eventByte: number; data: number[] };

export type Message =
  | { type: "ack"; id: number; queueUsed: number }
  | ({ type: "status"; id: number } & StatusDetail)
  | { type: "err"; id: number; code: number; codeName: string; arg: number }
  | ({ type: "event"; id: number } & EventDetail)
  | { type: "unknown"; id: number; op: number; data: number[] };

export type DecodeContext = {
  /** Which opcode a command id was sent with; picks the DONE body layout. Without it the layout is inferred from its length. */
  opOf?: (id: number) => number | undefined;
};

function decodeDone(
  id: number,
  rest: Uint8Array,
  ctx?: DecodeContext,
): DoneResult {
  const op = ctx?.opOf?.(id);
  const withState = (() => {
    const whole = new Uint8Array(rest.length + 1);
    whole[0] = STATE.DONE;
    whole.set(rest, 1);
    return whole;
  })();
  try {
    if (op === OP.HELLO || (op === undefined && rest.length === 38))
      return { kind: "info", info: decodeInfo(withState) };
    if (op === OP.STAT || (op === undefined && rest.length === 13))
      return { kind: "stat", stat: decodeStat(rest) };
    if (op === OP.ROW || (op === undefined && rest.length === 11))
      return { kind: "row", row: decodeRowResult(rest) };
    if (
      (op === OP.FLUSH || op === OP.ESTOP || op === undefined) &&
      rest.length === 1
    )
      return { kind: "flushed", flushed: rest[0]! };
    if ((op === OP.MOVE || op === undefined) && rest.length === 2)
      return { kind: "move", periodEffUs: rest[0]! * 256 + rest[1]! };
    if ((op === OP.CONFIG || op === undefined) && rest.length === 3)
      return {
        kind: "config",
        key: rest[0]!,
        value: rest[1]! * 256 + rest[2]!,
      };
    if (rest.length === 0) return { kind: "none" };
  } catch {}
  return { kind: "unknown", data: [...rest] };
}

/** Decode a STATUS frame's DATA. */
export function decodeStatus(f: Frame, ctx?: DecodeContext): StatusDetail {
  const d = f.data;
  const rest = d.subarray(1);
  switch (d[0]) {
    case STATE.QUEUED:
      return { state: "queued", pos: rest[0] ?? 0, queueUsed: rest[1] ?? 0 };
    case STATE.RUNNING:
      return { state: "running" };
    case STATE.DONE:
      return { state: "done", result: decodeDone(f.id, rest, ctx) };
    case STATE.FAILED:
      return {
        state: "failed",
        code: rest[0] ?? 0,
        codeName: nameOf(ERR_CODE, rest[0] ?? 0) ?? "?",
        arg: rest[1] ?? 0,
      };
    case STATE.CANCELLED:
      return {
        state: "cancelled",
        reason: rest[0] ?? 0,
        reasonName: nameOf(CANCEL_REASON, rest[0] ?? 0) ?? "?",
      };
    case STATE.PROGRESS:
      return {
        state: "progress",
        remaining:
          ((rest[0] ?? 0) * 0x1000000 +
            ((rest[1] ?? 0) << 16) +
            ((rest[2] ?? 0) << 8) +
            (rest[3] ?? 0)) >>>
          0,
      };
    default:
      return { state: "unknown", stateByte: d[0] ?? -1, data: [...d] };
  }
}

export function decodeEvent(f: Frame): EventDetail {
  const d = f.data;
  const rest = d.subarray(1);
  switch (d[0]) {
    case EVENT.HEARTBEAT:
      return { event: "heartbeat", stat: decodeStat(rest) };
    case EVENT.PAPER:
      return { event: "paper", present: rest[0] ?? 0xff };
    case EVENT.HOT:
      return { event: "hot", active: rest[0] === 1 };
    case EVENT.RESET:
      return { event: "reset", reason: rest[0] ?? 0 };
    case EVENT.STOPPED:
      return { event: "stopped", reason: rest[0] ?? 0 };
    default:
      return { event: "unknown", eventByte: d[0] ?? -1, data: [...d] };
  }
}

/** Hex line or Frame from the printer -> typed message. Throws only if the line is not a valid TBP frame. */
export function decodeMessage(
  input: string | Frame,
  ctx?: DecodeContext,
): Message {
  const f = typeof input === "string" ? decode(input) : input;
  switch (f.op) {
    case KIND.ACK:
      return { type: "ack", id: f.id, queueUsed: f.data[0] ?? 0 };
    case KIND.STATUS:
      return { type: "status", id: f.id, ...decodeStatus(f, ctx) };
    case KIND.ERR:
      return {
        type: "err",
        id: f.id,
        code: f.data[0] ?? 0,
        codeName: nameOf(ERR_CODE, f.data[0] ?? 0) ?? "?",
        arg: f.data[1] ?? 0,
      };
    case KIND.EVENT:
      return { type: "event", id: f.id, ...decodeEvent(f) };
    default:
      return { type: "unknown", id: f.id, op: f.op, data: [...f.data] };
  }
}
