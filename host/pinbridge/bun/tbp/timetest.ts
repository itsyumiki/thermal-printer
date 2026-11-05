// TIMETEST result -> verdict. Pure functions, no I/O. The firmware counts a source in its own units and
// says what it thinks that was worth (`wantUs`); the host measures real time between the RUNNING and DONE
// frames. ratio = wall / want: 1.0 means the timer is on time. Spec section 7.10.
import { TIME_SRC, nameOf } from "./protocol.ts";
import type { decodeTimeTestResult } from "./messages.ts";

/** Sprint 1 gate: every TIMER3 ratio within 2 % of 1.0. */
export const TIME_GATE = 0.02;

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
    /** Oscillator the wall time implies, TIMER3 only: ticks counted at the post-divider rate times div. */
    impliedOscMHz:
      t3 && wallMs > 0 ? (t.ticks * t.div) / (wallMs * 1000) : null,
    /** TIMER3 only: one compare must be exactly one ISR (false = a compare was counted twice or missed). */
    isrMatchesN: t3 ? t.isr === t.n : null,
    msPerJiffy: t.dj ? wallMs / t.dj : null,
    withinGate: Math.abs(ratio - 1) <= TIME_GATE && (t3 ? t.isr === t.n : true),
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
    ` -> ${s.withinGate ? "within 2 %" : "OUT OF GATE"}`
  );
}
