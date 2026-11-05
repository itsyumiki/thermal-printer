/*
 * Regression test: the 1 ms step-timer ISR preempting the command task while it starts or
 * extends a feed train (docs/bugs/2026-11-03-move-wedge-and-watchdog-cancel.md).
 *
 * pb_printer.h exposes PB_RACE_POINT(c, id) at the interesting points of pb_step_start and
 * pb_step_feed_schedule. This test defines it to run exactly one pb_step_tick there, which is
 * what the real timer interrupt can do. Every point is tried for MOVE and ROW at 1000/2000/6000 us,
 * for single commands and for back-to-back forward/reverse sequences that extend a live train.
 * A case passes when every command reports DONE and the motor ran exactly the requested steps.
 * The final case forces an unreachable target and expects FAILED/INTERNAL instead of a hang.
 *
 * Host-side only (no firmware build, no hardware). From the repo root:
 *   cc -std=gnu11 -O1 -Wall -I firmware/apps/pinbridge/src -o /tmp/pb_race_test \
 *      firmware/apps/pinbridge/tests/c/pb_race_test.c && /tmp/pb_race_test
 */
#ifndef PB_PINS_HEADER
#define PB_PINS_HEADER "pb_pins.h"
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void race_hook(void *core, int id);
#define PB_RACE_POINT(c, id) race_hook((c), (id))
#include "../../src/pb_core.h"

#define MAX_SITE 16
static unsigned now_ms;
static uint16_t outs[4], pads[4];
static char out[262144];
static size_t used;
static int armed_site;
static unsigned site_hits[MAX_SITE], total_site_hits[MAX_SITE];
static pb_core *active_core;
static unsigned latch_rises, clock_rises, heater_rises;
static uint32_t heater_edge_steps[8];
static void wr(void *x, unsigned p, uint16_t mask, uint16_t v);
static unsigned mock_heater_active, mock_heater_fail_start, mock_heater_long_waits;
static uint32_t mock_heater_deadline;
static int mock_heater_start(void *ctx, unsigned usec) {
    (void)ctx;
    if (mock_heater_fail_start || !usec || usec > PB_TUNE_HEATER_SLICE_MAX_US || mock_heater_active) return -1;
    mock_heater_active = 1;
    mock_heater_deadline = now_ms + (usec + 999u) / 1000u;
    wr(NULL, 1u, (uint16_t)(1u << 11), (uint16_t)(1u << 11));
    return 0;
}
static int mock_heater_active_poll(void *ctx) {
    (void)ctx;
    if (mock_heater_active && (int32_t)(now_ms - mock_heater_deadline) >= 0) {
        mock_heater_active = 0;
        wr(NULL, 1u, (uint16_t)(1u << 11), 0);
    }
    return (int)mock_heater_active;
}
static void mock_heater_cancel(void *ctx) {
    (void)ctx;
    mock_heater_active = 0;
    wr(NULL, 1u, (uint16_t)(1u << 11), 0);
}
static void observe_pin(unsigned port, unsigned bit, unsigned old_level, unsigned new_level) {
    if (!old_level && new_level && active_core && port == 1u) {
        if (bit == 8u) latch_rises++;
        if (bit == 9u) clock_rises++;
        if (bit == 11u && heater_rises < 8u)
            heater_edge_steps[heater_rises++] = active_core->step_completed;
    }
}

static void race_hook(void *core, int id) {
    if (id == armed_site) {
        site_hits[id]++;
        pb_step_tick((pb_core *)core); /* the 1 ms timer interrupt fires right here */
    }
}
static int mode(void *x, unsigned p, unsigned b, unsigned m, unsigned l) {
    (void)x; (void)m;
    unsigned old = (outs[p] >> b) & 1u;
    if (l) outs[p] |= (uint16_t)(1u << b); else outs[p] &= (uint16_t)~(1u << b);
    observe_pin(p, b, old, l);
    return 0;
}
static void wr(void *x, unsigned p, uint16_t mask, uint16_t v) {
    unsigned b;
    (void)x;
    for (b = 0; b < 16u; b++) if (mask & (1u << b)) {
        unsigned old = (outs[p] >> b) & 1u;
        unsigned next = (v >> b) & 1u;
        observe_pin(p, b, old, next);
    }
    outs[p] = (uint16_t)((outs[p] & ~mask) | (v & mask));
}
static uint16_t rd(void *x, unsigned p) { (void)x; return (uint16_t)(pads[p] | outs[p]); }
static int adc(void *x, int ch, unsigned *r, unsigned *m) {
    (void)x;
    if (r) *r = ch == -2 ? 500u : 412u;
    if (m) *m = ch == -2 ? 4000u : ch == 10 ? 1236u : 1219u;
    return 0;
}
static uint32_t ms(void *x) { (void)x; return now_ms; }
static uint32_t usec(void *x) { (void)x; return now_ms * 1000u; }
static void dly(void *x, unsigned us) { (void)x; if (us >= 1000u) mock_heater_long_waits++; }
static int tx(void *x, const char *s, size_t n) {
    (void)x;
    if (used + n < sizeof out - 1) { memcpy(out + used, s, n); used += n; out[used] = 0; }
    return 1;
}
static void send_frame(pb_core *c, uint8_t id, uint8_t op, const uint8_t *d, uint8_t len) {
    uint8_t raw[80];
    char hex[200];
    unsigned i;
    raw[0] = id; raw[1] = op; raw[2] = len;
    memcpy(raw + 3, d, len);
    {
        uint16_t crc = pb_tbp_crc(raw, (size_t)(3u + len));
        raw[3 + len] = (uint8_t)(crc >> 8);
        raw[4 + len] = (uint8_t)crc;
    }
    for (i = 0; i < 5u + len; i++) sprintf(hex + 2 * i, "%02X", raw[i]);
    strcat(hex, "\n");
    pb_rx(c, (const unsigned char *)hex, strlen(hex));
}
static void send_move(pb_core *c, uint8_t id, int32_t steps, unsigned per) {
    uint8_t d[6];
    d[0] = (uint8_t)((uint32_t)steps >> 24); d[1] = (uint8_t)((uint32_t)steps >> 16);
    d[2] = (uint8_t)((uint32_t)steps >> 8);  d[3] = (uint8_t)steps;
    d[4] = (uint8_t)(per >> 8); d[5] = (uint8_t)per;
    send_frame(c, id, 0x11, d, 6);
}
static void send_row(pb_core *c, uint8_t id, int steps, unsigned per) {
    uint8_t d[54];
    memset(d, 0, sizeof d); /* empty dot mask and zero burn: motion only, heater untouched */
    d[50] = (uint8_t)(int8_t)steps;
    d[51] = (uint8_t)(per >> 8); d[52] = (uint8_t)per;
    d[53] = 1;
    send_frame(c, id, 0x10, d, 54);
}
static void send_heated_row(pb_core *c, uint8_t id) {
    uint8_t d[54];
    memset(d, 0xff, 48); /* one full logical row */
    d[48] = 0x1f; d[49] = 0x40; /* 8000 us total / four slices */
    d[50] = 4; d[51] = 0x17; d[52] = 0x70; d[53] = 4; /* four edges @ 6 ms */
    send_frame(c, id, 0x10, d, 54);
}
static void run(pb_core *c, unsigned n) {
    while (n--) {
        now_ms++;
        pb_step_tick(c);
        if (now_ms % 10u == 0u) pb_poll(c);
    }
}
typedef struct { uint8_t done[256], failed[256], cancelled[256], fail_code[256]; } result;
static int hexv(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
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
                if (b[3] == 3u) r->done[b[0]] = 1;
                else if (b[3] == 4u) { r->failed[b[0]] = 1; r->fail_code[b[0]] = b[2] >= 2u ? b[4] : 0; }
                else if (b[3] == 5u) r->cancelled[b[0]] = 1;
            }
        }
        p = e + 1;
    }
}
enum { K_MOVE, K_ROW, K_HEATED_ROW, K_HEATED_PAIR, K_HEATER_FAIL, K_HEATER_CANCEL, K_MOVE_FWD_REV, K_MIXED, K_EXTEND_SWEEP, K_FORCED_UNREACHABLE, K_COUNT };
static const char *const kname[K_COUNT] = {"MOVE +80", "ROW +40", "heated ROW four synchronized slices", "two continuous heated rows", "heater timer start failure", "heater timer cancellation", "MOVE +80 then -80", "ROW+30,MOVE-30,ROW+30", "extend near end of train", "forced unreachable target"};
static unsigned variant; /* K_EXTEND_SWEEP: start offset (0..9 ms) and how close to the train end the 2nd MOVE arrives */

static int trial(int kind, unsigned per, int site, int verbose) {
    static pb_core c;
    pb_hal h = {0, mode, wr, rd, adc, 0, ms, tx, dly, wr,
                mock_heater_start, mock_heater_active_poll, mock_heater_cancel, 0, 0, usec};
    result r;
    unsigned per_ms = (per + 999u) / 1000u, expect_ids = 0, total_steps = 0, i;
    int ok = 1, forced = 0, cancel_sent = 0;
    memset(&c, 0, sizeof c); memset(outs, 0, sizeof outs);
    active_core = &c; latch_rises = clock_rises = heater_rises = 0;
    mock_heater_active = mock_heater_fail_start = mock_heater_long_waits = 0;
    mock_heater_deadline = 0;
    memset(heater_edge_steps, 0, sizeof heater_edge_steps);
    used = 0; out[0] = 0; now_ms = 1000; armed_site = 0; memset(site_hits, 0, sizeof site_hits);
    pb_init(&c, &h);
    { uint8_t d[1] = {1}; send_frame(&c, 1, 0x01, d, 1); } /* HELLO */
    run(&c, 50); used = 0; out[0] = 0;
    latch_rises = clock_rises = heater_rises = 0;
    mock_heater_active = mock_heater_fail_start = mock_heater_long_waits = 0;
    mock_heater_deadline = 0;
    memset(heater_edge_steps, 0, sizeof heater_edge_steps);
    armed_site = site;
    switch (kind) {
    case K_MOVE: send_move(&c, 2, 80, per); expect_ids = 1; total_steps = 80; break;
    case K_ROW: send_row(&c, 2, 40, per); expect_ids = 1; total_steps = 40; break;
    case K_HEATED_ROW:
        send_heated_row(&c, 2); expect_ids = 1; total_steps = 4; per = 6000u; per_ms = 6u; break;
    case K_HEATED_PAIR:
        send_heated_row(&c, 2); send_heated_row(&c, 3);
        expect_ids = 2; total_steps = 8; per = 6000u; per_ms = 6u; break;
    case K_HEATER_FAIL:
        mock_heater_fail_start = 1; send_heated_row(&c, 2);
        total_steps = 4; per = 6000u; per_ms = 6u; break;
    case K_HEATER_CANCEL:
        send_heated_row(&c, 2); total_steps = 4; per = 6000u; per_ms = 6u; break;
    case K_FORCED_UNREACHABLE: send_move(&c, 2, 80, per); break;
    case K_MOVE_FWD_REV: send_move(&c, 2, 80, per); run(&c, 20); send_move(&c, 3, -80, per); expect_ids = 2; total_steps = 160; break;
    case K_EXTEND_SWEEP:
        /* The second MOVE arrives -25..+5 ms around the end of the first train, at every poll phase,
         * so the extend path runs while the last ISR tick can still end the train. */
        run(&c, variant % 10u); send_move(&c, 2, 80, per);
        run(&c, 80u * per_ms - 25u + 5u * (variant / 10u)); send_move(&c, 3, -80, per);
        expect_ids = 2; total_steps = 160; break;
    case K_MIXED:
        send_row(&c, 2, 30, per); run(&c, 10); send_move(&c, 3, -30, per); run(&c, 10);
        send_row(&c, 4, 30, per); expect_ids = 3; total_steps = 90; break;
    }
    {
    for (i = 0; i < total_steps * per_ms + (kind == K_FORCED_UNREACHABLE ? 1500u : 2500u); i++) {
        now_ms++;
        pb_step_tick(&c);
        if ((kind == K_HEATED_ROW || kind == K_HEATED_PAIR || kind == K_HEATER_FAIL || kind == K_HEATER_CANCEL) || now_ms % 10u == 0u) pb_poll(&c);
        if (kind == K_HEATER_CANCEL && mock_heater_active && !cancel_sent) {
            send_frame(&c, 3, 0x03, NULL, 0); /* STOP while hardware pulse is active */
            cancel_sent = 1;
        }
        if (kind == K_FORCED_UNREACHABLE && !forced && c.tbp_running && c.step_active && c.tbp_feed_train_active) {
            c.tbp_feed_target += 1u; /* the pre-fix off-by-one: a target the train can never reach */
            forced = 1;
        }
    }
    }
    parse(&r);
    if (kind == K_HEATER_FAIL) {
        if (!r.failed[2] || r.done[2] || mock_heater_active || heater_rises ||
            (outs[1] & (1u << 11)) || c.step_active) ok = 0;
    } else if (kind == K_HEATER_CANCEL) {
        if (!cancel_sent || !r.cancelled[2] || !r.done[3] || mock_heater_active ||
            (outs[1] & (1u << 11)) || c.step_active) ok = 0;
    } else if (kind == K_FORCED_UNREACHABLE) {
        if (!forced || !r.failed[2] || r.fail_code[2] != 0x1Fu || r.done[2]) ok = 0;
    } else {
        unsigned id;
        for (id = 2; id < 2u + expect_ids; id++) if (!r.done[id] || r.failed[id] || r.cancelled[id]) ok = 0;
        if (c.step_completed != total_steps || c.step_active) ok = 0;
    }
    if (kind == K_HEATED_ROW || kind == K_HEATED_PAIR) {
        unsigned j, rows = kind == K_HEATED_PAIR ? 2u : 1u;
        if (latch_rises != rows || clock_rises != 384u * rows || heater_rises != 4u * rows ||
            (outs[1] & (1u << 11)) != 0u) ok = 0;
        for (j = 0; j < 4u * rows; j++)
            if (heater_edge_steps[j] != j + 1u) ok = 0;
        if (mock_heater_long_waits) ok = 0; /* no blocking delay >= 1 ms in the pulse path */
    }
    for (i = 0; i < MAX_SITE; i++) total_site_hits[i] += site_hits[i];
    if (!ok || verbose)
        printf("%s kind=\"%s\" period=%uus site=%d hits=%u: completed=%u target=%u active=%u (expected %u steps)\n",
               ok ? "PASS" : "FAIL", kname[kind], per, site, site ? site_hits[site] : 0u,
               (unsigned)c.step_completed, (unsigned)c.tbp_feed_target, (unsigned)c.step_active, total_steps);
    if ((kind == K_HEATED_ROW || kind == K_HEATED_PAIR || kind == K_HEATER_FAIL || kind == K_HEATER_CANCEL) && (!ok || verbose))
        printf("  row observations: latch=%u clocks=%u heater=%u stage=%u slices=%u flags=%u\n",
               latch_rises, clock_rises, heater_rises, c.tbp_exec_stage, c.tbp_exec_slice, c.tbp_row_flags);
    if ((kind == K_HEATED_ROW || kind == K_HEATED_PAIR || kind == K_HEATER_FAIL || kind == K_HEATER_CANCEL) && (!ok || verbose)) {
        unsigned j; printf("  heater steps:"); for (j = 0; j < heater_rises; j++) printf(" %lu", (unsigned long)heater_edge_steps[j]); printf("\n");
    }
    return ok;
}
int main(int argc, char **argv) {
    static const unsigned periods[] = {1000u, 2000u, 6000u};
    static const int sites[] = {0, 1, 2, 3, 4, 5, 10, 11, 12};
    static const int required[] = {1, 2, 3, 4, 5, 10, 11, 12};
    unsigned pi, si, ki, cases = 0, fails = 0;
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    for (ki = 0; ki < K_COUNT; ki++)
        for (pi = 0; pi < sizeof periods / sizeof *periods; pi++)
            for (si = 0; si < sizeof sites / sizeof *sites; si++) {
                unsigned nv = ki == K_EXTEND_SWEEP ? 70u : 1u;
                if (ki == K_FORCED_UNREACHABLE && sites[si] != 0) continue;
                if ((ki == K_HEATED_ROW || ki == K_HEATED_PAIR || ki == K_HEATER_FAIL || ki == K_HEATER_CANCEL) && (pi != 0u || si != 0u)) continue;
                for (variant = 0; variant < nv; variant++) {
                    cases++;
                    if (!trial((int)ki, periods[pi], sites[si], verbose)) fails++;
                }
            }
    for (si = 0; si < sizeof required / sizeof *required; si++)
        if (!total_site_hits[required[si]]) {
            printf("FAIL race point %d was never reached: the test no longer covers it\n", required[si]);
            fails++;
        }
    printf("%u cases, %u failed\n", cases, fails);
    return fails ? 1 : 0;
}
