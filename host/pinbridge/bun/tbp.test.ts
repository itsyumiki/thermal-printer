import { describe, expect, test } from "bun:test";
import {
  crc16,
  encode,
  decode,
  OP,
  row,
  move,
  decodeInfo,
  MAX_HEATER_SLICE_US,
} from "./tbp.ts";

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
  test("accepts CRLF framing and event ID zero on receive", () => {
    const frame = encode(1, OP.HELLO, Uint8Array.of(1)).trim();
    expect(decode(frame + "\r").id).toBe(1);
    const event = encode(1, 0x84, Uint8Array.of(1)).replace(/^01/, "00");
    // Recompute CRC after changing the event id to its reserved value.
    const raw = Uint8Array.from(event.trim().match(/../g)!, (x) =>
      parseInt(x, 16),
    );
    const crc = crc16(raw.subarray(0, -2));
    raw[raw.length - 2] = crc >> 8;
    raw[raw.length - 1] = crc & 255;
    const wire = [...raw].map((b) => b.toString(16).padStart(2, "0")).join("");
    expect(decode(wire).id).toBe(0);
  });
  test("rejects corrupt CRC and malformed lengths", () => {
    const frame = encode(1, OP.HELLO, Uint8Array.of(1)).trim();
    expect(() => decode(frame.slice(0, -2) + "00")).toThrow();
    expect(() => decode("010101")).toThrow();
    expect(() => decode(" " + frame)).toThrow();
    expect(() => decode(frame.slice(0, 4) + "GG" + frame.slice(6))).toThrow();
    expect(() => decode("01010101E655")).toThrow();
    expect(() => decode("AA".repeat(261))).toThrow();
    expect(() => encode(0, OP.HELLO, Uint8Array.of(1))).toThrow();
    expect(decode(frame + "\r").id).toBe(1);
  });
  test("enforces the per-slice heater ceiling while allowing a four-slice row total", () => {
    const mask = new Uint8Array(48).fill(255);
    expect(MAX_HEATER_SLICE_US).toBe(2500);
    expect(row(mask, 8000, 8, 6000, 4)[53]).toBe(4);
    expect(() => row(mask, 5000, 8, 6000, 1)).toThrow("2500 µs");
    expect(() => row(mask, 10000, 8, 6000, 3)).toThrow("2500 µs");
  });
  test("encodes signed motion in two's complement", () => {
    expect([...move(-200, 3000)]).toEqual([255, 255, 255, 56, 11, 184]);
  });
  test("decodes 38-byte INFO payload", () => {
    const info = Uint8Array.of(3, ...new Uint8Array(38));
    info[1] = 1;
    info[2] = 0x12;
    info[3] = 0x34;
    info[4] = 0x56;
    info[5] = 0x78;
    info[6] = 1;
    info[7] = 128;
    const parsed = decodeInfo(info);
    expect(parsed.version).toBe(1);
    expect(parsed.buildId).toBe(0x12345678);
    expect(parsed.dots).toBe(384);
  });
});
