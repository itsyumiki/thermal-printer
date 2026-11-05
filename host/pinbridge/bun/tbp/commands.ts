// Host -> printer: typed commands <-> frames. Pure functions, no I/O.
import { CONFIG_KEY, OP } from "./protocol.ts";
import {
  decode,
  encode,
  fromHex,
  i32,
  join,
  u16,
  type Frame,
} from "./frame.ts";

export const DEFAULT_HEATER_SLICES = 1;
export const DOTS = 384;
export const MASK_BYTES = DOTS / 8;

/** A row mask: 48 raw bytes, 96 hex characters, or all-black / all-white. */
export type Mask = Uint8Array | string | "black" | "white";

export type Command =
  | { type: "hello"; version?: number }
  | { type: "stat" }
  | { type: "flush" }
  | { type: "estop" }
  | { type: "config"; key: number; value: number }
  | {
      type: "row";
      mask: Mask;
      burnUs: number;
      steps: number;
      periodUs: number;
      slices?: number;
    }
  | { type: "move"; steps: number; periodUs: number }
  | { type: "dwell"; ms: number }
  | { type: "coils"; on: boolean };

export function maskBytes(mask: Mask): Uint8Array {
  if (mask instanceof Uint8Array) return mask;
  if (mask === "black") return new Uint8Array(MASK_BYTES).fill(0xff);
  if (mask === "white") return new Uint8Array(MASK_BYTES);
  return fromHex(mask);
}

export function row(
  mask: Uint8Array,
  burnUs: number,
  steps: number,
  periodUs: number,
  slices = DEFAULT_HEATER_SLICES,
): Uint8Array {
  if (mask.length !== MASK_BYTES)
    throw new Error("A print row must be exactly 48 bytes (384 dots)");
  if (!Number.isInteger(burnUs) || burnUs < 0 || burnUs > 65535)
    throw new Error(
      "Row burn time must fit the 16-bit wire field (0..65535 µs); the firmware enforces the real heat limits",
    );
  if (!Number.isInteger(steps) || steps < -128 || steps > 127)
    throw new Error("Row steps must be -128..127");
  if (steps && periodUs < 1000)
    throw new Error(
      "Moving rows require a period of at least 1000 µs on this firmware",
    );
  if (steps && periodUs > 65000)
    throw new Error("Step period must not exceed 65000 µs on this firmware");
  if (
    !Number.isInteger(slices) ||
    slices < 1 ||
    slices > Math.max(1, Math.abs(steps))
  )
    throw new Error("Invalid heat slices");
  return join(
    mask,
    u16(burnUs),
    Uint8Array.of(steps & 255),
    u16(periodUs),
    Uint8Array.of(slices),
  );
}

export function move(steps: number, periodUs: number): Uint8Array {
  if (!steps) throw new Error("MOVE steps cannot be zero");
  if (periodUs < 1000 || periodUs > 65000)
    throw new Error("Step period must be 1000..65000 µs on this firmware");
  return join(i32(steps), u16(periodUs));
}

/** The opcode and DATA bytes for a command. Throws if a field is out of range. */
export function commandPayload(cmd: Command): { op: number; data: Uint8Array } {
  switch (cmd.type) {
    case "hello":
      return { op: OP.HELLO, data: Uint8Array.of(cmd.version ?? 1) };
    case "stat":
      return { op: OP.STAT, data: new Uint8Array() };
    case "flush":
      return { op: OP.FLUSH, data: new Uint8Array() };
    case "estop":
      return { op: OP.ESTOP, data: new Uint8Array() };
    case "config":
      if (!Number.isInteger(cmd.key) || cmd.key < 0 || cmd.key > 255)
        throw new Error("CONFIG key must be 0..255");
      return {
        op: OP.CONFIG,
        data: join(Uint8Array.of(cmd.key), u16(cmd.value)),
      };
    case "row":
      return {
        op: OP.ROW,
        data: row(
          maskBytes(cmd.mask),
          cmd.burnUs,
          cmd.steps,
          cmd.periodUs,
          cmd.slices ?? DEFAULT_HEATER_SLICES,
        ),
      };
    case "move":
      return { op: OP.MOVE, data: move(cmd.steps, cmd.periodUs) };
    case "dwell":
      if (!Number.isInteger(cmd.ms) || cmd.ms < 1 || cmd.ms > 65535)
        throw new Error("DWELL must be 1..65535 ms");
      return { op: OP.DWELL, data: u16(cmd.ms) };
    case "coils":
      return { op: OP.COILS, data: Uint8Array.of(cmd.on ? 1 : 0) };
  }
}

/** Command -> hex line (with trailing LF) for a given command id. */
export function encodeCommand(cmd: Command, id: number): string {
  const { op, data } = commandPayload(cmd);
  return encode(id, op, data);
}

/**
 * Hex frame -> command: the inverse of encodeCommand(). Accepts a decoded Frame or a hex line.
 * Throws on a bad CRC, an unknown opcode or a wrong payload length.
 */
export function decodeCommand(input: string | Frame): {
  id: number;
  command: Command;
} {
  const f = typeof input === "string" ? decode(input) : input;
  const d = f.data;
  const need = (n: number) => {
    if (d.length !== n)
      throw new Error(
        `Opcode 0x${f.op.toString(16)} needs ${n} data byte(s), got ${d.length}`,
      );
  };
  const u16at = (i: number) => d[i]! * 256 + d[i + 1]!;
  switch (f.op) {
    case OP.HELLO:
      need(1);
      return { id: f.id, command: { type: "hello", version: d[0]! } };
    case OP.STAT:
      need(0);
      return { id: f.id, command: { type: "stat" } };
    case OP.FLUSH:
      need(0);
      return { id: f.id, command: { type: "flush" } };
    case OP.ESTOP:
      need(0);
      return { id: f.id, command: { type: "estop" } };
    case OP.CONFIG:
      need(3);
      return {
        id: f.id,
        command: { type: "config", key: d[0]!, value: u16at(1) },
      };
    case OP.ROW:
      need(54);
      return {
        id: f.id,
        command: {
          type: "row",
          mask: d.slice(0, MASK_BYTES),
          burnUs: u16at(48),
          steps: (d[50]! << 24) >> 24,
          periodUs: u16at(51),
          slices: d[53]!,
        },
      };
    case OP.MOVE:
      need(6);
      return {
        id: f.id,
        command: {
          type: "move",
          steps: (d[0]! << 24) | (d[1]! << 16) | (d[2]! << 8) | d[3]! | 0,
          periodUs: u16at(4),
        },
      };
    case OP.DWELL:
      need(2);
      return { id: f.id, command: { type: "dwell", ms: u16at(0) } };
    case OP.COILS:
      need(1);
      return { id: f.id, command: { type: "coils", on: d[0] === 1 } };
    default:
      throw new Error(`Unknown opcode 0x${f.op.toString(16).padStart(2, "0")}`);
  }
}

/**
 * Hands out command ids the way the spec wants them: HELLO is id 1 and restarts the sequence,
 * everything else takes the next id (1..255, wrapping, never 0), and ESTOP reuses the next id
 * without advancing it. Use it to script a printer without any server.
 */
export class Encoder {
  nextId = 1;
  encode(cmd: Command): { id: number; line: string } {
    let id = this.nextId;
    if (cmd.type === "hello") id = 1;
    const line = encodeCommand(cmd, id);
    if (cmd.type !== "estop") this.nextId = id >= 255 ? 1 : id + 1;
    return { id, line };
  }
}

export { CONFIG_KEY };
