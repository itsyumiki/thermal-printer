// TBP v1 framing: `<ID><OP><LEN><DATA><CRC16>` as uppercase hex, one frame per line.
// Pure functions, no I/O.

export type Frame = { id: number; op: number; data: Uint8Array };

/** 520 hex characters (255 data bytes); the wire line adds one LF. */
export const MAX_HEX_CHARS = 520;

/** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0. */
export function crc16(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++)
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

export function toHex(bytes: Uint8Array | number[]): string {
  return [...bytes]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("")
    .toUpperCase();
}

/** Parse ASCII hex (either case, even length, no separators). Throws on anything else. */
export function fromHex(hex: string): Uint8Array {
  if (hex.length % 2 || !/^[0-9a-f]*$/i.test(hex))
    throw new Error("Invalid hex string");
  return Uint8Array.from(hex.match(/../g) ?? [], (x) => parseInt(x, 16));
}

/** Encode one frame, including the trailing LF. Throws if a field is out of range. */
export function encode(
  id: number,
  op: number,
  data = new Uint8Array(),
): string {
  if (id < 1 || id > 255 || op < 0 || op > 255 || data.length > 255)
    throw new Error("TBP frame field out of range");
  return encodeRaw(id, op, data);
}

/** Like encode() but lets the printer-originated id 0 through (events, test vectors). */
export function encodeRaw(
  id: number,
  op: number,
  data = new Uint8Array(),
): string {
  if (!(id >= 0 && id <= 255) || op < 0 || op > 255 || data.length > 255)
    throw new Error("TBP frame field out of range");
  const raw = new Uint8Array(5 + data.length);
  raw[0] = id;
  raw[1] = op;
  raw[2] = data.length;
  raw.set(data, 3);
  const crc = crc16(raw.subarray(0, raw.length - 2));
  raw[raw.length - 2] = crc >> 8;
  raw[raw.length - 1] = crc & 255;
  return toHex(raw) + "\n";
}

/** Decode one line (a trailing LF or CRLF is accepted). Throws on bad hex, length or CRC. */
export function decode(line: string): Frame {
  const withoutLf = line.endsWith("\n") ? line.slice(0, -1) : line;
  const s = withoutLf.endsWith("\r") ? withoutLf.slice(0, -1) : withoutLf;
  if (!s || s.length % 2 || s.length > MAX_HEX_CHARS || !/^[0-9a-f]+$/i.test(s))
    throw new Error("Invalid TBP hex frame");
  const raw = fromHex(s);
  if (raw.length < 5 || raw[2] !== raw.length - 5)
    throw new Error("TBP length mismatch");
  if (
    crc16(raw.subarray(0, -2)) !==
    raw[raw.length - 2]! * 256 + raw[raw.length - 1]!
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
