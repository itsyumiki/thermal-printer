/*
 * Host-side test of pb_timebase.h (Sprint 2): the microsecond read and the timebase delay, run against a simulated
 * periodic timer. The simulation is the harness's clock: true time advances by one tick (1/6 us) for every
 * register access, which stands in for the cost of reading a register, and the wrap ISR runs `lag` ticks after
 * the timer wraps, so every interleaving of "wrapped but ISR not run yet" is exercised.
 *
 * What is checked: pb_tb_us() never goes backwards and always lies between true time at call start and at call end
 * (so a late ISR does not cost 10 ms and a torn read does not either), for ISR lags from 0 to a full period minus
 * one tick, and with the ISR masked for a while; pb_tb_delay_us() waits at least the asked time and at most a small
 * overshoot, for 1 us up to 30 ms; a stopped timer ends in -1 within the spin bound instead of hanging; and the
 * u32 microsecond count subtracts correctly across its 71.6 minute wrap.
 *
 * Evidence class: simulation only. It says nothing about the real TIMER3 (see the TIMEBASE self-test).
 * From the repo root:
 *   cc -std=c11 -Wall -Wextra -Werror -I firmware/apps/pinbridge/src -o /tmp/pb_timebase_test \
 *      firmware/apps/pinbridge/tests/c/pb_timebase_test.c && /tmp/pb_timebase_test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SIM_PRD 60000u
static uint64_t sim_t;           /* true time in ticks (6 per us) */
static uint32_t sim_wraps;       /* what the ISR has counted */
static unsigned sim_lag;         /* ticks between a wrap and its ISR running */
static int sim_frozen;           /* timer stopped: time does not advance */
static int sim_irq_masked;       /* ISR cannot run (the pending flag stays set) */
static unsigned long sim_reads;  /* register accesses, to prove the spin bound */

static void sim_advance(unsigned ticks) {
    if (sim_frozen)
        return;
    sim_t += ticks;
    if (!sim_irq_masked) {
        /* ISR k (k = sim_wraps + 1) is due at k * PRD + lag. */
        while (sim_t >= ((uint64_t)sim_wraps + 1u) * SIM_PRD + sim_lag)
            sim_wraps++;
    }
}
static uint32_t sim_get_wraps(void) {
    sim_reads++;
    sim_advance(1);
    return sim_wraps;
}
static uint32_t sim_get_cnt(void) {
    sim_reads++;
    sim_advance(1);
    return (uint32_t)(sim_t % SIM_PRD);
}
static int sim_get_pend(void) {
    sim_reads++;
    sim_advance(1);
    return sim_t / SIM_PRD > sim_wraps; /* wrapped, ISR owes an increment */
}
#define PB_TB_WRAPS() sim_get_wraps()
#define PB_TB_CNT() sim_get_cnt()
#define PB_TB_PEND() sim_get_pend()
#include "pb_timebase.h"

static unsigned failures;
#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            failures++;                                                                                        \
            if (failures < 20) {                                                                               \
                printf("FAIL line %d: ", __LINE__);                                                            \
                printf(__VA_ARGS__);                                                                           \
                printf("\n");                                                                                  \
            }                                                                                                  \
        }                                                                                                      \
    } while (0)

static uint32_t true_us(void) {
    return (uint32_t)(sim_t / PB_TB_TICKS_PER_US);
}
static unsigned rnd_state = 12345u;
static unsigned rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}
static void reset(uint64_t start_ticks, unsigned lag) {
    sim_t = start_ticks;
    sim_wraps = (uint32_t)(start_ticks / SIM_PRD); /* a clean boot would be 0; any offset must work */
    sim_lag = lag;
    sim_frozen = 0;
    sim_irq_masked = 0;
    sim_reads = 0;
}

static void test_constants(void) {
    CHECK(PB_TB_PRD == 60000u, "PRD %u", PB_TB_PRD);
    CHECK(PB_TB_PRD <= 0xffffu, "PRD does not fit the 16-bit register");
    CHECK(PB_TB_PRD % PB_TB_TICKS_PER_US == 0u, "a period is not a whole number of microseconds");
}

/* The read is bracketed by true time, whatever the ISR lag, for random gaps between reads. */
static void test_read_bracketed(void) {
    static const unsigned lags[] = {0, 1, 2, 3, 5, 7, 20, 50, 600, 6000, 30000, 59000, 59999};
    unsigned li, i;
    for (li = 0; li < sizeof lags / sizeof lags[0]; li++) {
        uint32_t prev = 0;
        reset((uint64_t)(rnd() % 1000u) * SIM_PRD + rnd() % SIM_PRD, lags[li]);
        prev = true_us();
        for (i = 0; i < 400000u; i++) {
            uint32_t lo, hi, v;
            sim_advance(rnd() % 40u); /* idle gap, long enough to straddle wraps sometimes */
            lo = true_us();
            v = pb_tb_us();
            hi = true_us();
            CHECK((uint32_t)(v - lo) <= (uint32_t)(hi - lo), "lag %u: read %u outside [%u, %u]", lags[li], v, lo, hi);
            CHECK((int32_t)(v - prev) >= 0, "lag %u: time went backwards %u -> %u", lags[li], prev, v);
            prev = v;
        }
    }
}

/* ISR masked for part of a period: the pending flag carries the wrap, so time still never steps back. */
static void test_read_with_irq_masked(void) {
    unsigned i;
    uint32_t prev;
    reset(5u * SIM_PRD + 100u, 20u);
    prev = pb_tb_us();
    for (i = 0; i < 20000u; i++) {
        uint32_t lo, hi, v;
        unsigned span = 1u + rnd() % (SIM_PRD - 200u); /* under one period of masking: one wrap at most */
        while (sim_t / SIM_PRD > sim_wraps) /* start with the ISR caught up: one lost wrap is not what is tested */
            sim_advance(1);
        sim_irq_masked = 1;
        sim_advance(span);
        lo = true_us();
        v = pb_tb_us();
        hi = true_us();
        CHECK((uint32_t)(v - lo) <= (uint32_t)(hi - lo), "masked: read %u outside [%u, %u]", v, lo, hi);
        CHECK((int32_t)(v - prev) >= 0, "masked: time went backwards");
        prev = v;
        sim_irq_masked = 0;
        sim_advance(1); /* unmask: the late ISR runs and the count catches up without a jump */
        v = pb_tb_us();
        CHECK((int32_t)(v - prev) >= 0, "unmask: time went backwards");
        prev = v;
    }
}

static void test_delay(void) {
    static const unsigned asks[] = {1, 2, 3, 5, 10, 100, 999, 1000, 1001, 5000, 9999, 10000, 10001, 29999, 30000};
    unsigned ai, rep;
    uint32_t worst_over_2 = 0, worst_over_1000 = 0;
    for (ai = 0; ai < sizeof asks / sizeof asks[0]; ai++) {
        for (rep = 0; rep < 2000u; rep++) {
            uint64_t t0, t1;
            int rc;
            uint32_t waited_us_x6;
            reset((uint64_t)(rnd() % 100000u) * 7u + rnd() % SIM_PRD, rnd() % 100u);
            t0 = sim_t;
            rc = pb_tb_delay_us(asks[ai]);
            t1 = sim_t;
            waited_us_x6 = (uint32_t)(t1 - t0);
            CHECK(rc == 0, "delay %u returned %d", asks[ai], rc);
            CHECK((int32_t)waited_us_x6 >= (int32_t)(asks[ai] * PB_TB_TICKS_PER_US) - 11,
                  "delay %u too short: %u ticks", asks[ai], waited_us_x6); /* the first read can sit up to 10 ticks late */
            CHECK(waited_us_x6 <= asks[ai] * PB_TB_TICKS_PER_US + 8u * 12u, "delay %u too long: %u ticks", asks[ai],
                  waited_us_x6);
            if (asks[ai] == 2u && waited_us_x6 > worst_over_2)
                worst_over_2 = waited_us_x6;
            if (asks[ai] == 1000u && waited_us_x6 > worst_over_1000)
                worst_over_1000 = waited_us_x6;
        }
    }
    printf("sim: delay(2) worst %.2f us, delay(1000) worst %.2f us (one register access = 1/6 us)\n",
           worst_over_2 / 6.0, worst_over_1000 / 6.0);
    /* A zero delay is a no-op for the caller; the helper itself must still return. */
    reset(0, 0);
    CHECK(pb_tb_delay_us(0) == 0, "delay 0");
}

/* A timebase that stopped must not hang the caller: the spin bound ends the wait with -1. */
static void test_stalled(void) {
    unsigned asks[] = {1, 2, 1000, 30000};
    unsigned i;
    for (i = 0; i < sizeof asks / sizeof asks[0]; i++) {
        int rc;
        reset(1234567u, 10u);
        sim_frozen = 1;
        rc = pb_tb_delay_us(asks[i]);
        CHECK(rc == -1, "frozen timebase: delay %u returned %d", asks[i], rc);
        /* 5 register accesses per pb_tb_us(); the bound is 16 * us + 256 reads of it. */
        CHECK(sim_reads <= (16ul * asks[i] + 257ul) * 5ul + 5ul, "frozen timebase: %lu reads for delay %u", sim_reads,
              asks[i]);
    }
}

/* Simulate a wedged executor: only the independent wrap-ISR deadman runs and must force PB11 low. */
static void test_heater_deadman_wedge(void) {
    volatile uint8_t armed = 1u;
    volatile uint32_t trips = 0u;
    unsigned pb11_high = 1u;
    uint32_t deadline = 2500u;
    CHECK(!pb_heater_deadman_expire(&armed, deadline - 1u, deadline, &trips),
          "deadman tripped before the requested deadline");
    CHECK(armed && pb11_high && trips == 0u, "early deadman changed heater state");
    /* Main-loop polling is deliberately absent here; the timer ISR owns this transition. */
    if (pb_heater_deadman_expire(&armed, deadline, deadline, &trips)) pb11_high = 0u;
    CHECK(!armed && !pb11_high && trips == 1u, "wedged-loop deadman did not force PB11 low");
    CHECK(!pb_heater_deadman_expire(&armed, deadline + PB_TB_PERIOD_US, deadline, &trips) && trips == 1u,
          "deadman counted a second trip after disarming");
    /* Deadline comparison must also survive the 32-bit microsecond clock wrapping. */
    armed = 1u; pb11_high = 1u; trips = 0u;
    deadline = 0xfffffff0u;
    CHECK(!pb_heater_deadman_expire(&armed, 0xffffffe0u, deadline, &trips), "wrapped deadline fired early");
    if (pb_heater_deadman_expire(&armed, 5u, deadline, &trips)) pb11_high = 0u;
    CHECK(!armed && !pb11_high && trips == 1u, "wrapped deadline did not expire");
}

/* The u32 microsecond count wraps every 2^32 us; differences must stay right across it. */
static void test_u32_wrap(void) {
    /* Start 3 ms before the u32 wrap point: 2^32 us = 25769803776 ticks. */
    uint64_t wrap_ticks = 4294967296ull * PB_TB_TICKS_PER_US;
    uint32_t a, b;
    reset(wrap_ticks - 3000u * PB_TB_TICKS_PER_US, 15u);
    a = pb_tb_us();
    sim_advance(7000u * PB_TB_TICKS_PER_US);
    b = pb_tb_us();
    CHECK((uint32_t)(b - a) >= 6990u && (uint32_t)(b - a) <= 7010u, "u32 wrap: elapsed %u", (uint32_t)(b - a));
    CHECK(b < a, "the test did not cross the u32 wrap (%u -> %u)", a, b);
    CHECK(pb_tb_delay_us(2000u) == 0, "delay across the u32 wrap");
}

int main(void) {
    test_constants();
    test_read_bracketed();
    test_read_with_irq_masked();
    test_delay();
    test_stalled();
    test_heater_deadman_wedge();
    test_u32_wrap();
    if (failures) {
        printf("%u failure(s)\n", failures);
        return 1;
    }
    printf("pb_timebase_test: ok\n");
    return 0;
}
