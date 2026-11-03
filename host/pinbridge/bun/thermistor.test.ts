import { describe, expect, test } from "bun:test";
import { headTempCFromRaw } from "./thermistor.ts";

describe("stock head thermistor conversion", () => {
  test("matches firmware table conversion for room-temperature samples", () => {
    expect(headTempCFromRaw(370)).toBe(24);
    expect(headTempCFromRaw(412)).toBe(20);
    expect(headTempCFromRaw(357)).toBe(25);
  });

  test("fails closed for short, open, sentinel and malformed readings", () => {
    expect(headTempCFromRaw(20)).toBeNull();
    expect(headTempCFromRaw(900)).toBeNull();
    expect(headTempCFromRaw(65535)).toBeNull();
    expect(headTempCFromRaw(-1)).toBeNull();
    expect(headTempCFromRaw(370.5)).toBeNull();
  });
});
