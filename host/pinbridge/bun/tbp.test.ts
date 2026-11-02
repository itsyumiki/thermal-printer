import { describe, expect, test } from "bun:test";
import { crc16, encode, decode, OP, row, move, decodeInfo } from "./tbp.ts";

describe("TBP v1 framing", () => {
  test("CRC-16/CCITT-FALSE check vector", () =>
    expect(crc16(new TextEncoder().encode("123456789"))).toBe(0x29b1));
  test("matches the documented HELLO frame", () =>
    expect(encode(1, OP.HELLO, Uint8Array.of(1))).toBe("01010101E654\n"));
  test("round-trips a frame and verifies its CRC", () => {
    const wire = encode(
      5,
      OP.ROW,
      row(new Uint8Array(48).fill(0xaa), 1500, 8, 6000, 4),
    );
    const f = decode(wire);
    expect(f.id).toBe(5);
    expect(f.op).toBe(OP.ROW);
    expect(f.data.length).toBe(54);
    expect(f.data[48]).toBe(5);
    expect(f.data[49]).toBe(220);
    expect(f.data[50]).toBe(8);
    expect(f.data[51]).toBe(0x17);
    expect(f.data[52]).toBe(0x70);
    expect(f.data[53]).toBe(4);
  });
  test("rejects corrupt CRC and malformed lengths", () => {
    const frame = encode(1, OP.HELLO, Uint8Array.of(1)).trim();
    expect(() => decode(frame.slice(0, -2) + "00")).toThrow();
    expect(() => decode("010101")).toThrow();
    expect(() => decode(" " + frame)).toThrow();
    expect(() => decode("AA".repeat(261))).toThrow();
    expect(() => encode(0, OP.HELLO, Uint8Array.of(1))).toThrow();
    expect(decode(frame + "\r").id).toBe(1);
  });
  test("encodes signed motion in two's complement", () => {
    expect([...move(-200, 3000)]).toEqual([255, 255, 255, 56, 11, 184]);
  });
  test("decodes 38-byte INFO payload", () => {
    const info = Uint8Array.of(3, ...new Uint8Array(38));
    info[1] = 1;
    info[6] = 1;
    info[7] = 128;
    const parsed = decodeInfo(info);
    expect(parsed.version).toBe(1);
    expect(parsed.dots).toBe(384);
  });
});
