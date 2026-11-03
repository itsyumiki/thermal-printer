// Paper-feed units. The firmware counts half-steps and a per-half-step period in microseconds;
// the host and UI speak millimetres and mm/s. Mechanism: 160 half-steps = 5 mm.
export const STEPS_PER_MM = 32;
export const MIN_PERIOD_US = 1000; // firmware floor (PB_TUNE_STEPPER_MIN_PERIOD_US)
export const MAX_PERIOD_US = 65000;
export const MAX_ROW_STEPS = 127; // ROW feed is an int8 of half-steps

/** mm of paper -> half-steps (nearest integer; the sign gives the direction). */
export function mmToSteps(mm: number): number {
  if (!Number.isFinite(mm)) throw new Error("Distance must be a number of mm");
  return Math.round(mm * STEPS_PER_MM);
}

export function stepsToMm(steps: number): number {
  return steps / STEPS_PER_MM;
}

/** Fastest and slowest feed speed the firmware accepts, in mm/s. */
export const MAX_SPEED_MM_S = 1e6 / MIN_PERIOD_US / STEPS_PER_MM; // 31.25
export const MIN_SPEED_MM_S = 1e6 / MAX_PERIOD_US / STEPS_PER_MM; // ~0.48

/** Feed speed in mm/s -> microseconds per half-step. Throws outside the firmware's range. */
export function speedToPeriodUs(mmPerSecond: number): number {
  if (!Number.isFinite(mmPerSecond) || mmPerSecond <= 0)
    throw new Error("Speed must be a positive number of mm/s");
  const period = Math.round(1e6 / (mmPerSecond * STEPS_PER_MM));
  if (period < MIN_PERIOD_US || period > MAX_PERIOD_US)
    throw new Error(
      `Speed must be between ${MIN_SPEED_MM_S.toFixed(2)} and ${MAX_SPEED_MM_S.toFixed(2)} mm/s`,
    );
  return period;
}

export function periodUsToSpeed(periodUs: number): number {
  return 1e6 / periodUs / STEPS_PER_MM;
}

/**
 * Resolve a motion request. Messages may carry millimetres (distanceMm / speedMmS, what the UI
 * sends) or the raw firmware units (steps / periodUs, kept for scripts and tests).
 * Millimetres win when both are present.
 */
export function motionFromMessage(
  msg: any,
  defaults: { steps?: number; periodUs: number },
): { steps: number; periodUs: number } {
  const steps =
    msg?.distanceMm !== undefined && msg.distanceMm !== null
      ? mmToSteps(Number(msg.distanceMm))
      : Number(msg?.steps ?? defaults.steps);
  const periodUs =
    msg?.speedMmS !== undefined && msg.speedMmS !== null
      ? speedToPeriodUs(Number(msg.speedMmS))
      : Number(msg?.periodUs ?? defaults.periodUs);
  return { steps, periodUs };
}
