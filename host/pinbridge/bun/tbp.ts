export const OP = {
  HELLO: 0x01,
  STAT: 0x02,
  FLUSH: 0x03,
  CONFIG: 0x04,
  ROW: 0x10,
  MOVE: 0x11,
  DWELL: 0x12,
  COILS: 0x20,
  ESTOP: 0xff,
} as const;
export const KIND = {
  ACK: 0x81,
  STATUS: 0x82,
  ERR: 0x83,
  EVENT: 0x84,
} as const;
export const STATE = {
  QUEUED: 1,
  RUNNING: 2,
  DONE: 3,
  FAILED: 4,
  CANCELLED: 5,
  PROGRESS: 6,
} as const;

/** Temporary timing-test envelope; firmware still enforces row-total and thermal/duty gates. */
export const MAX_HEATER_SLICE_US = 10000;
export const DEFAULT_HEATER_SLICES = 1;
export type Frame = { id: number; op: number; data: Uint8Array };
export function crc16(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++)
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}
export function encode(
  id: number,
  op: number,
  data = new Uint8Array(),
): string {
  if (id < 1 || id > 255 || op < 0 || op > 255 || data.length > 255)
    throw new Error("TBP frame field out of range");
  const raw = new Uint8Array(5 + data.length);
  raw[0] = id;
  raw[1] = op;
  raw[2] = data.length;
  raw.set(data, 3);
  const crc = crc16(raw.subarray(0, raw.length - 2));
  raw[raw.length - 2] = crc >> 8;
  raw[raw.length - 1] = crc & 255;
  return (
    [...raw]
      .map((b) => b.toString(16).padStart(2, "0"))
      .join("")
      .toUpperCase() + "\n"
  );
}
export function decode(line: string): Frame {
  const withoutLf = line.endsWith("\n") ? line.slice(0, -1) : line;
  const s = withoutLf.endsWith("\r") ? withoutLf.slice(0, -1) : withoutLf;
  if (!s || s.length % 2 || s.length > 520 || !/^[0-9a-f]+$/i.test(s))
    throw new Error("Invalid TBP hex frame");
  const raw = Uint8Array.from(s.match(/../g)!, (x) => parseInt(x, 16));
  if (raw.length < 5 || raw[2] !== raw.length - 5)
    throw new Error("TBP length mismatch");
  if (
    crc16(raw.subarray(0, -2)) !==
    raw[raw.length - 2] * 256 + raw[raw.length - 1]
  )
    throw new Error("TBP CRC mismatch");
  return { id: raw[0]!, op: raw[1]!, data: raw.slice(3, -2) };
}
export function u16(v: number): Uint8Array {
  if (!Number.isInteger(v) || v < 0 || v > 65535)
    throw new Error("Expected u16");
  return Uint8Array.of(v >> 8, v & 255);
}
export function i32(v: number): Uint8Array {
  if (!Number.isInteger(v) || v < -2147483648 || v > 2147483647)
    throw new Error("Expected i32");
  const n = v >>> 0;
  return Uint8Array.of(n >>> 24, (n >>> 16) & 255, (n >>> 8) & 255, n & 255);
}
export function join(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let offset = 0;
  for (const p of parts) {
    out.set(p, offset);
    offset += p.length;
  }
  return out;
}
export function row(
  mask: Uint8Array,
  burnUs: number,
  steps: number,
  periodUs: number,
  slices = 1,
): Uint8Array {
  if (mask.length !== 48)
    throw new Error("A print row must be exactly 48 bytes (384 dots)");
  if (!Number.isInteger(burnUs) || burnUs < 0 || burnUs > 65535)
    throw new Error(
      "Row burn time must be 0..65535 µs (the firmware enforces its own INFO limits)",
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
  if (Math.ceil(burnUs / slices) > MAX_HEATER_SLICE_US)
    throw new Error(
      `Each heat slice must be at most ${MAX_HEATER_SLICE_US} µs`,
    );
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
export function decodeInfo(data: Uint8Array) {
  if (data.length !== 39 || data[0] !== STATE.DONE)
    throw new Error(`Invalid HELLO STATUS payload (${data.length} bytes)`);
  const b = data.subarray(1);
  const n = (i: number) => b[i]! * 256 + b[i + 1]!;
  return {
    version: b[0],
    buildId: (b[1]! * 0x1000000 + (b[2]! << 16) + (b[3]! << 8) + b[4]!) >>> 0,
    dots: n(5),
    queueDepth: b[7],
    maxWindow: b[8],
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
    caps: b[29],
    uid: [...b.slice(30, 38)]
      .map((x) => x.toString(16).padStart(2, "0"))
      .join(""),
  };
}
