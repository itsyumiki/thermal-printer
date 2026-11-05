// TIMETEST result -> verdict. Pure functions, no I/O. The firmware counts a source in its own units and
// says what it thinks that was worth (`wantUs`); the host measures real time between the RUNNING and DONE
// frames. ratio = wall / want: 1.0 means the timer is on time. Spec section 7.10.
import { TIME_SRC, nameOf } from "./protocol.ts";
import type { decodeTimeTestResult } from "./messages.ts";

/** Sprint 1 gate: every TIMER3 ratio within 2 % of 1.0. */
export const TIME_GATE = 0.02;
/** Sprint 2 gate for the timebase against the host clock: within 1 %. */
export const TIME_GATE_TIMEBASE = 0.01;
/** The ratio tolerance that applies to a source (the other sources keep the 2 % figure; see the sprint docs). */
const gateOf = (src: string) =>
  src === "TIMEBASE" ? TIME_GATE_TIMEBASE : TIME_GATE;

export function summarizeTimeTest(
  t: ReturnType<typeof decodeTimeTestResult>,
  wallMs: number,
) {
  const src = nameOf(TIME_SRC, t.src) ?? `src${t.src}`;
  const wantMs = t.wantUs / 1000;
  const ratio = wantMs > 0 ? wallMs / wantMs : NaN;
  const t3 = t.div > 0;
  return {
    src,
    wallMs,
    wantMs,
    ratio,
    gate: gateOf(src),
    /** Oscillator the wall time implies, TIMER3 only: ticks counted at the post-divider rate times div. */
    impliedOscMHz:
      t3 && wallMs > 0 ? (t.ticks * t.div) / (wallMs * 1000) : null,
    /** TIMER3 only: one compare must be exactly one ISR (false = a compare was counted twice or missed). */
    isrMatchesN: t3 ? t.isr === t.n : null,
    msPerJiffy: t.dj ? wallMs / t.dj : null,
    withinGate:
      Math.abs(ratio - 1) <= gateOf(src) && (t3 ? t.isr === t.n : true),
  };
}

/** One log line for a finished TIMETEST. */
export function describeTimeTestSummary(
  s: ReturnType<typeof summarizeTimeTest>,
): string {
  const f = (v: number | null, d: number) => (v === null ? "-" : v.toFixed(d));
  return (
    `TIMETEST ${s.src}: wall ${s.wallMs.toFixed(1)} ms vs want ${s.wantMs.toFixed(1)} ms, ratio ${s.ratio.toFixed(4)}` +
    (s.impliedOscMHz !== null ? `, osc ${f(s.impliedOscMHz, 2)} MHz` : "") +
    (s.isrMatchesN !== null ? `, isr==n ${s.isrMatchesN ? "yes" : "NO"}` : "") +
    (s.msPerJiffy !== null ? `, ${f(s.msPerJiffy, 2)} ms/jiffy` : "") +
    ` -> ${s.withinGate ? `within ${(s.gate * 100).toFixed(0)} %` : "OUT OF GATE"}`
  );
}

/** One finished TIMETEST run: the firmware's decoded result plus the host's wall time between RUNNING and DONE. */
export type TimeTestRun = {
  test: ReturnType<typeof decodeTimeTestResult>;
  wallMs: number;
};

const mean = (a: number[]) => a.reduce((s, x) => s + x, 0) / a.length;
const stdev = (a: number[]) =>
  a.length < 2
    ? 0
    : Math.sqrt(a.reduce((s, x) => s + (x - mean(a)) ** 2, 0) / (a.length - 1));

/**
 * Several runs of one source, folded into the figures the web UI shows. `ratio` is the mean wall time over what
 * the firmware thinks it counted; `withinGate` also needs one ISR per counted TIMER3 period in every run.
 */
export function summarizeTimeTestRuns(runs: TimeTestRun[]) {
  if (runs.length === 0) throw new Error("no TIMETEST runs to summarize");
  const per = runs.map((r) => summarizeTimeTest(r.test, r.wallMs));
  const wall = per.map((s) => s.wallMs);
  const avg = (
    f: (s: (typeof per)[number], r: TimeTestRun) => number | null,
  ) => {
    const v = per
      .map((s, i) => f(s, runs[i]!))
      .filter((x): x is number => x !== null && Number.isFinite(x));
    return v.length ? mean(v) : null;
  };
  const first = per[0]!;
  const meanWallMs = mean(wall);
  return {
    src: first.src,
    runs: runs.length,
    wantMs: first.wantMs,
    meanWallMs,
    stdevMs: stdev(wall),
    minWallMs: Math.min(...wall),
    maxWallMs: Math.max(...wall),
    ratio: first.wantMs > 0 ? meanWallMs / first.wantMs : NaN,
    gate: first.gate,
    nPerRun: runs[0]!.test.n,
    /** What one counted unit is really worth (ms): a step tick, a jiffy, a delay call or a TIMER3 period. */
    msPerUnit: avg((s, r) => (r.test.n > 0 ? s.wallMs / r.test.n : null)),
    impliedOscMHz: avg((s) => s.impliedOscMHz),
    msPerJiffy: avg((s) => s.msPerJiffy),
    isrPerRun: first.isrMatchesN === null ? null : avg((_s, r) => r.test.isr),
    isrMatchesN:
      first.isrMatchesN === null ? null : per.every((s) => s.isrMatchesN),
    withinGate:
      Number.isFinite(meanWallMs / first.wantMs) &&
      Math.abs(meanWallMs / first.wantMs - 1) <= first.gate &&
      per.every((s) => s.isrMatchesN !== false),
  };
}
