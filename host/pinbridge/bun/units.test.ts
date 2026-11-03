import { describe, expect, test } from "bun:test";
import {
  MAX_SPEED_MM_S,
  effectivePeriodUs,
  mmToSteps,
  motionFromMessage,
  periodUsToSpeed,
  speedToPeriodUs,
  stepsToMm,
} from "./units.ts";

describe("paper-feed units", () => {
  test("160 half-steps are 5 mm", () => {
    expect(mmToSteps(5)).toBe(160);
    expect(mmToSteps(-5)).toBe(-160);
    expect(stepsToMm(160)).toBe(5);
    expect(mmToSteps(2.5)).toBe(80);
    expect(mmToSteps(0.25)).toBe(8);
  });

  test("speed and period convert both ways", () => {
    expect(MAX_SPEED_MM_S).toBe(31.25);
    expect(speedToPeriodUs(31.25)).toBe(1000);
    expect(periodUsToSpeed(1000)).toBe(31.25);
    expect(speedToPeriodUs(15.625)).toBe(2000);
    expect(speedToPeriodUs(15)).toBe(3000);
    expect(effectivePeriodUs(1042)).toBe(2000);
    expect(periodUsToSpeed(1042)).toBe(15.625);
    expect(periodUsToSpeed(6000)).toBeCloseTo(5.2083, 3);
    expect(speedToPeriodUs(periodUsToSpeed(6000))).toBe(6000);
  });

  test("rejects speeds the firmware cannot run", () => {
    expect(() => speedToPeriodUs(31.5)).toThrow("mm/s");
    expect(() => speedToPeriodUs(0.1)).toThrow("mm/s");
    expect(() => speedToPeriodUs(0)).toThrow("mm/s");
    expect(() => speedToPeriodUs(NaN)).toThrow("mm/s");
    expect(() => mmToSteps(NaN)).toThrow("mm");
  });

  test("messages accept mm, mm/s, or raw steps/us; mm wins", () => {
    expect(
      motionFromMessage(
        { distanceMm: 2.5, speedMmS: 31.25 },
        { periodUs: 6000 },
      ),
    ).toEqual({ steps: 80, periodUs: 1000 });
    expect(
      motionFromMessage({ steps: 80, periodUs: 3000 }, { periodUs: 6000 }),
    ).toEqual({ steps: 80, periodUs: 3000 });
    expect(
      motionFromMessage({ steps: 80, periodUs: 1042 }, { periodUs: 6000 }),
    ).toEqual({ steps: 80, periodUs: 2000 });
    expect(motionFromMessage({}, { steps: 8, periodUs: 1000 })).toEqual({
      steps: 8,
      periodUs: 1000,
    });
    expect(
      motionFromMessage(
        { distanceMm: -5, steps: 99, speedMmS: 15.625, periodUs: 1 },
        { periodUs: 1000 },
      ),
    ).toEqual({ steps: -160, periodUs: 2000 });
  });
});
