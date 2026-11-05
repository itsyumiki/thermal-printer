/* pb_timebase.h - free-running microsecond clock built from one periodic hardware timer (Sprint 2).
 *
 * Portable on purpose: app.c includes it for the real TIMER3, and tests/c/pb_timebase_test.c includes it
 * against a simulated timer, so the code that is tested is the code that is flashed.
 *
 * The includer defines, before including this file:
 *   PB_TB_WRAPS()  number of period interrupts the ISR has serviced so far (u32, volatile read)
 *   PB_TB_CNT()    the timer's 16-bit counter, which counts 0..PB_TB_PRD-1 and then wraps
 *   PB_TB_PEND()   non-zero while the timer has wrapped but its ISR has not yet run (CON bit 15)
 * The ISR does `wraps++` and then clears the pending flag, in that order.
 *
 * Configuration (see docs/timer-architecture-and-usage.md, "Timebase"): 24 MHz oscillator / 4 = 6 ticks per us,
 * PRD 60000 = one wrap every 10 ms exactly. Resolution is 1/6 us; the u32 microsecond count wraps every
 * 71.6 min, so callers must subtract (now - start) and never compare two raw values.
 */
#ifndef PB_TIMEBASE_H
#define PB_TIMEBASE_H
#include <stdint.h>

#define PB_TB_TICKS_PER_US 6u
#define PB_TB_PERIOD_US 10000u
#define PB_TB_PRD (PB_TB_PERIOD_US * PB_TB_TICKS_PER_US) /* 60000, fits the 16-bit PRD register */

/* Microseconds since the timebase started. Lock-free and safe from any context, including with interrupts
 * masked and from inside an ISR. A reading that straddles a wrap is retried: the wrap count must not change
 * and the counter must not run backwards between the two counter reads. A wrap whose ISR has not run yet
 * (pending flag set) is added in here, so the result never steps back by 10 ms while an ISR is late.
 * Limit: if interrupts stay masked for more than one period (10 ms) a wrap is lost and time steps back. */
/* Shared deadman decision used by the TIMER3 ISR and host tests. Signed subtraction keeps
 * deadlines valid across the u32 microsecond-clock wrap for intervals shorter than 2^31 us. */
static inline int pb_heater_deadman_expire(volatile uint8_t *armed, uint32_t now, uint32_t deadline,
                                           volatile uint32_t *trips) {
    if (!*armed || (int32_t)(now - deadline) < 0) return 0;
    *armed = 0;
    if (trips) (*trips)++;
    return 1;
}

static inline uint32_t pb_tb_us(void) {
    uint32_t w, cnt, cnt2, pend;
    unsigned tries;
    w = 0;
    cnt = 0;
    pend = 0;
    for (tries = 0; tries < 8u; tries++) {
        uint32_t w1 = PB_TB_WRAPS();
        cnt = PB_TB_CNT();
        pend = PB_TB_PEND() ? 1u : 0u;
        cnt2 = PB_TB_CNT();
        w = PB_TB_WRAPS();
        if (w == w1 && cnt2 >= cnt)
            break;
    }
    return (w + pend) * PB_TB_PERIOD_US + cnt / PB_TB_TICKS_PER_US;
}

/* Busy-wait until `us` microseconds of timebase time have passed. Returns 0, or -1 if the timebase stopped
 * advancing (a healthy loop needs at most a few passes per microsecond, so 16 passes per us plus 256 is far
 * beyond it). The caller then finishes with its software loop and stops trusting the timebase. */
static inline int pb_tb_delay_us(unsigned us) {
    uint32_t start = pb_tb_us();
    unsigned long spins = (unsigned long)us * 16ul + 256ul;
    while ((uint32_t)(pb_tb_us() - start) < us) {
        if (!--spins)
            return -1;
    }
    return 0;
}
#endif
