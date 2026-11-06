/*
 * Regression test for the feed look-ahead (pb_tbp_prefeed_next in pb_core.h): the stepper must not
 * wait for the heater.
 *
 * Before the look-ahead, the executor appended a row's feed to the phase train only when it started
 * that row, and it started the next row only after the previous row's last heater pulse. A pulse
 * that outlasted the final step dwell let the train run dry, so every heated row restarted the motor
 * (ROW flag COLD_START). The existing harnesses cannot see this: their pulse loops run with the
 * step ISR frozen. This one models the real hardware: simulated microseconds advance inside every
 * micros()/delay_us() call, and the 1 ms step tick fires at each millisecond boundary, also while
 * PB11 is high. The executor is polled every 100 us, the way a Sprint 4 busy loop would.
 *
 * Cases:
 *   1. Six queued heated rows whose 2 ms pulses are longer than the 1 ms step period: one train
 *      start, COLD_START only on the first row, all 24 pulses at the requested width.
 *   2. A row that would have to wait for the duty budget is NOT fed early: it waits with the motor
 *      idle and reports DUTY_WAIT, so the print stays position-correct.
 *   3. Cancelling while rows are fed ahead stops the motor at once, and the next row starts clean.
 *   4. The look-ahead does not change what a row reports when it runs alone.
 *   5. A heat-gate failure (open thermistor) while rows are fed ahead stops the motor and cancels them.
 *
 * Host-side only (no firmware build, no hardware). From the repo root:
 *   cc -std=gnu11 -O1 -Wall -I firmware/apps/pinbridge/src -o /tmp/pb_prefeed_test \
 *      firmware/apps/pinbridge/tests/c/pb_prefeed_test.c && /tmp/pb_prefeed_test
 */
#ifndef PB_PINS_HEADER
#define PB_PINS_HEADER "pb_pins.h"
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/pb_core.h"

static uint64_t sim_us;
static pb_core *cur;
static uint16_t outs[4], pads[4];
static char out[262144];
static size_t used;
static unsigned train_starts, prev_active;
static unsigned pulses, pulse_min_us, pulse_max_us;
static uint64_t pulse_rise_us;
static unsigned stage7_motor_active;
static int failures;
static int temp_sensor_open; /* case 5: the head thermistor reads open-circuit */

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void note_train(void) {
    if (cur->step_active && !prev_active) train_starts++;
    prev_active = cur->step_active;
}
static void sim_adv(unsigned us) {
    while (us--) {
        sim_us++;
        if (sim_us % 1000u == 0u && cur) {
            pb_step_tick(cur); /* the 1 ms timer interrupt: it runs while a pulse is high, too */
            note_train();
        }
    }
}
static void observe_pin(unsigned port, unsigned bit, unsigned old_level, unsigned new_level) {
    if (port != 1u || bit != 11u) return;
    if (!old_level && new_level) pulse_rise_us = sim_us;
    if (old_level && !new_level) {
        unsigned w = (unsigned)(sim_us - pulse_rise_us);
        pulses++;
        if (!pulse_min_us || w < pulse_min_us) pulse_min_us = w;
        if (w > pulse_max_us) pulse_max_us = w;
    }
}
static int mode(void *x, unsigned p, unsigned b, unsigned m, unsigned l) {
    unsigned old = (outs[p] >> b) & 1u;
    (void)x; (void)m;
    if (l) outs[p] |= (uint16_t)(1u << b); else outs[p] &= (uint16_t)~(1u << b);
    observe_pin(p, b, old, l);
    return 0;
}
static void wr(void *x, unsigned p, uint16_t mask, uint16_t v) {
    unsigned b;
    (void)x;
    for (b = 0; b < 16u; b++) if (mask & (1u << b)) observe_pin(p, b, (outs[p] >> b) & 1u, (v >> b) & 1u);
    outs[p] = (uint16_t)((outs[p] & ~mask) | (v & mask));
}
static uint16_t rd(void *x, unsigned p) { (void)x; return (uint16_t)(pads[p] | outs[p]); }
static int adc(void *x, int ch, unsigned *r, unsigned *m) {
    (void)x;
    if (r) *r = ch == -2 ? 500u : (ch == 10 && temp_sensor_open) ? 950u : 412u;
    if (m) *m = ch == -2 ? 4000u : ch == 10 ? 1236u : 1219u;
    return 0;
}
static uint32_t ms(void *x) { (void)x; return (uint32_t)(sim_us / 1000u); }
static uint32_t usec(void *x) { (void)x; sim_adv(1); return (uint32_t)sim_us; } /* a read costs about 1 us */
static void dly(void *x, unsigned us) { (void)x; sim_adv(us); }
static int arm(void *x, unsigned us) { (void)x; (void)us; return 0; }
static void disarm(void *x) { (void)x; }
static int tx(void *x, const char *s, size_t n) {
    (void)x;
    if (used + n < sizeof out - 1) { memcpy(out + used, s, n); used += n; out[used] = 0; }
    return 1;
}
static void send_frame(pb_core *c, uint8_t id, uint8_t op, const uint8_t *d, uint8_t len) {
    uint8_t raw[80];
    char hex[200];
    unsigned i;
    uint16_t crc;
    raw[0] = id; raw[1] = op; raw[2] = len;
    memcpy(raw + 3, d, len);
    crc = pb_tbp_crc(raw, (size_t)(3u + len));
    raw[3 + len] = (uint8_t)(crc >> 8);
    raw[4 + len] = (uint8_t)crc;
    for (i = 0; i < 5u + len; i++) sprintf(hex + 2 * i, "%02X", raw[i]);
    strcat(hex, "\n");
    pb_rx(c, (const unsigned char *)hex, strlen(hex));
}
static void send_row(pb_core *c, uint8_t id, int heated, unsigned burn_us, int steps, unsigned period_us, unsigned slices) {
    uint8_t d[54];
    memset(d, heated ? 0xff : 0x00, 48);
    d[48] = (uint8_t)(burn_us >> 8); d[49] = (uint8_t)burn_us;
    d[50] = (uint8_t)(int8_t)steps;
    d[51] = (uint8_t)(period_us >> 8); d[52] = (uint8_t)period_us;
    d[53] = (uint8_t)slices;
    send_frame(c, id, 0x10, d, 54);
}
static int hexv(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
typedef struct { uint8_t done[256], failed[256], cancelled[256], flags[256]; } result;
static void parse(result *r) {
    char *p = out, *e;
    memset(r, 0, sizeof *r);
    while ((e = strchr(p, '\n')) != 0) {
        size_t n = (size_t)(e - p), i;
        uint8_t b[96];
        int ok = n >= 10 && n % 2 == 0 && n / 2 <= sizeof b;
        for (i = 0; ok && i < n; i++) if (hexv(p[i]) < 0) ok = 0;
        if (ok) {
            for (i = 0; i < n / 2; i++) b[i] = (uint8_t)(hexv(p[2 * i]) * 16 + hexv(p[2 * i + 1]));
            if (b[1] == 0x82 && b[2] >= 1u && n / 2 == (size_t)(5u + b[2])) {
                if (b[3] == 3u) { r->done[b[0]] = 1; if (b[2] == 13u) r->flags[b[0]] = b[15]; }
                else if (b[3] == 4u) r->failed[b[0]] = 1;
                else if (b[3] == 5u) r->cancelled[b[0]] = 1;
            }
        }
        p = e + 1;
    }
}
static void run_us(unsigned total_us) {
    uint64_t end = sim_us + total_us;
    while (sim_us < end) {
        pb_poll(cur);
        note_train();
        if (cur->tbp_running && cur->tbp_exec_stage == 7u && cur->step_active) stage7_motor_active++;
        sim_adv(100);
    }
}
static pb_core core;
static void start(void) {
    pb_hal h = {.set_mode = mode, .write_port = wr, .write_port_fast = wr, .read_port = rd, .adc_read = adc,
                .millis = ms, .tx_write = tx, .delay_us = dly, .heater_deadman_arm = arm,
                .heater_deadman_disarm = disarm, .micros = usec};
    memset(&core, 0, sizeof core);
    memset(outs, 0, sizeof outs);
    cur = &core;
    sim_us = 1000000u;
    used = 0; out[0] = 0;
    train_starts = prev_active = 0;
    pulses = pulse_min_us = pulse_max_us = 0;
    stage7_motor_active = 0;
    temp_sensor_open = 0;
    pb_init(&core, &h);
    { uint8_t d[1] = {1}; send_frame(&core, 1, 0x01, d, 1); } /* HELLO */
    run_us(50000);
    used = 0; out[0] = 0;
    train_starts = 0; prev_active = core.step_active;
    pulses = pulse_min_us = pulse_max_us = 0;
}

static void case_pulses_longer_than_the_step_period(void) {
    result r;
    unsigned id;
    uint32_t base;
    printf("case 1: six queued heated rows, one 2 ms pulse per row after its last step edge, 1 ms step period\n");
    start();
    base = core.step_completed;
    for (id = 2; id <= 7; id++) send_row(&core, (uint8_t)id, 1, 2000u, 4, 1000u, 1);
    run_us(400000);
    parse(&r);
    for (id = 2; id <= 7; id++) CHECK(r.done[id], "row %u did not finish", id);
    CHECK(core.step_completed - base == 24u, "motor ran %u steps, expected 24", (unsigned)(core.step_completed - base));
    CHECK(train_starts == 1u, "phase train started %u times, expected once (the motor stopped between rows)", train_starts);
    CHECK(r.flags[2] & PB_TBP_ROWFLAG_COLD_START, "first row should report COLD_START");
    for (id = 3; id <= 7; id++) CHECK(!(r.flags[id] & PB_TBP_ROWFLAG_COLD_START), "row %u restarted the motor (flags 0x%02x)", id, r.flags[id]);
    for (id = 2; id <= 7; id++) CHECK(!(r.flags[id] & (PB_TBP_ROWFLAG_DUTY_WAIT | PB_TBP_ROWFLAG_HOT_WAIT)), "row %u waited (flags 0x%02x)", id, r.flags[id]);
    CHECK(pulses == 6u, "%u heater pulses, expected 6", pulses);
    CHECK(pulse_min_us >= 1990u, "shortest pulse %u us, expected 2000", pulse_min_us);
    CHECK(pulse_max_us <= 2100u, "longest pulse %u us, expected 2000", pulse_max_us);
}
static void case_duty_wait_is_not_fed_early(void) {
    result r;
    uint32_t base;
    printf("case 2: a row that must wait for the duty budget is not fed early\n");
    start();
    base = core.step_completed;
    send_row(&core, 2, 1, 30000u, 4, 6000u, 4);
    send_row(&core, 3, 1, 30000u, 4, 6000u, 4);
    send_row(&core, 4, 1, 30000u, 4, 6000u, 4); /* would exceed the 60 ms per 200 ms budget */
    run_us(1500000);
    parse(&r);
    CHECK(r.done[2] && r.done[3] && r.done[4], "rows did not all finish");
    CHECK(core.step_completed - base == 12u, "motor ran %u steps, expected 12", (unsigned)(core.step_completed - base));
    CHECK(r.flags[4] & PB_TBP_ROWFLAG_DUTY_WAIT, "third row should report DUTY_WAIT (flags 0x%02x)", r.flags[4]);
    CHECK(stage7_motor_active == 0u, "the motor was running while a row sat in DUTY_WAIT (%u samples)", stage7_motor_active);
    CHECK(!(r.flags[3] & PB_TBP_ROWFLAG_COLD_START), "second row should chain onto the first train (flags 0x%02x)", r.flags[3]);
}
static void case_cancel_drops_the_look_ahead(void) {
    result r;
    uint32_t at_cancel;
    printf("case 3: cancel while rows are fed ahead\n");
    start();
    send_row(&core, 2, 1, 4000u, 4, 6000u, 4);
    send_row(&core, 3, 1, 4000u, 4, 6000u, 4);
    send_row(&core, 4, 1, 4000u, 4, 6000u, 4);
    run_us(10000);
    pb_tbp_cancel_work(&core, 1u);
    at_cancel = core.step_completed;
    run_us(300000);
    CHECK(!core.step_active, "motor still active after cancel");
    CHECK(core.step_completed == at_cancel, "motor kept stepping after cancel (%u more steps)", (unsigned)(core.step_completed - at_cancel));
    CHECK(core.tbp_qcount == 0u && !core.tbp_running, "queue not empty after cancel");
    used = 0; out[0] = 0;
    send_row(&core, 5, 1, 4000u, 4, 6000u, 4);
    run_us(300000);
    parse(&r);
    CHECK(r.done[5], "row after the cancel did not finish");
    CHECK(core.step_completed - at_cancel == 4u, "row after the cancel moved %u steps, expected 4", (unsigned)(core.step_completed - at_cancel));
    CHECK(r.flags[5] & PB_TBP_ROWFLAG_COLD_START, "row after the cancel should start cold (flags 0x%02x)", r.flags[5]);
}
static void case_single_row_unchanged(void) {
    result r;
    printf("case 4: a lone row reports what it always did\n");
    start();
    send_row(&core, 2, 1, 8000u, 4, 6000u, 4);
    run_us(200000);
    parse(&r);
    CHECK(r.done[2], "row did not finish");
    CHECK((r.flags[2] & ~PB_TBP_ROWFLAG_LATE) == PB_TBP_ROWFLAG_COLD_START, "flags 0x%02x, expected COLD_START (LATE is set by the old code too)", r.flags[2]);
    CHECK(pulses == 4u, "%u pulses, expected 4", pulses);
    CHECK(core.step_completed >= 4u, "motor did not move");
}
static void case_heater_failure_flushes_the_look_ahead(void) {
    result r;
    uint32_t at_fail;
    printf("case 5: a heat-gate failure while a row is fed ahead stops the motor\n");
    start();
    send_row(&core, 2, 1, 2000u, 4, 6000u, 4);
    send_row(&core, 3, 1, 2000u, 4, 6000u, 4);
    send_row(&core, 4, 1, 2000u, 4, 6000u, 4);
    run_us(8000);
    CHECK(core.step_active, "precondition: motor should be running with rows fed ahead");
    temp_sensor_open = 1; /* the next slice gate re-samples and must fail closed */
    run_us(60000);
    parse(&r);
    at_fail = core.step_completed;
    CHECK(r.failed[2], "row 2 should fail on the open thermistor");
    CHECK(!core.step_active, "motor still active after the failure");
    CHECK(core.tbp_qcount == 0u && !core.tbp_running, "queue not empty after the failure");
    CHECK(r.cancelled[3] && r.cancelled[4], "rows fed ahead should be cancelled, not finished");
    run_us(300000);
    CHECK(core.step_completed == at_fail, "motor kept stepping after the failure (%u more steps)", (unsigned)(core.step_completed - at_fail));
}
int main(void) {
    case_pulses_longer_than_the_step_period();
    case_duty_wait_is_not_fed_early();
    case_cancel_drops_the_look_ahead();
    case_single_row_unchanged();
    case_heater_failure_flushes_the_look_ahead();
    if (failures) {
        printf("pb_prefeed_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("pb_prefeed_test: ok\n");
    return 0;
}
