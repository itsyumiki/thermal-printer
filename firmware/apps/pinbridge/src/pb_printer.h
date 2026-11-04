/* Internal to pb_core.h; shares its core state and reply helpers. */
#ifndef PB_PRINTER_H
#define PB_PRINTER_H

/* Printer-specific commands and the nonblocking motor stepper. */
static const char *const pb_motor_pins[4] = {"PA9", "PA10", "PC3", "PC2"};
/* Official JieLi AC6956C half-step sequence on PA9, PA10, PC3, PC2.
 Owner confirms the phase pin mapping and motor function; phase timing/drive limits still require firmware-specific validation. */
static const uint8_t pb_motor_phases[8][4] = {{0, 1, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 1},
                                              {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0},
                                              {1, 0, 1, 0}, {0, 0, 1, 0}};

/* Timer self-test sources for PTIME. Order matters: app.c switches on these indices.
 * T3Dn  = heater timer (TIMER3) at divider n, compare mode, counting a fixed number of compare periods
 * STEP  = SDK 1 ms hi-res timer (the stepper tick), counting callbacks
 * JIFFY = SDK jiffies counter, assumed 10 ms per jiffy
 * DELAY / DELAY2 = hal_delay_us(1000) / hal_delay_us(2) called back to back */
#define PB_TIME_NSRC 8u
static const char *const pb_time_srcs[PB_TIME_NSRC] = {"T3D4", "T3D16", "T3D64", "T3D256",
                                                         "STEP", "JIFFY", "DELAY", "DELAY2"};
#define PB_TIME_MS_MIN 100u
#define PB_TIME_MS_MAX 3000u
#define PB_TIME_MS_DEFAULT 1000u

static int pb_hexbyte(const char *s, unsigned *out) {
    unsigned v = 0, i;
    for (i = 0; i < 2; i++) {
        char x = s[i];
        unsigned d;
        if (x >= '0' && x <= '9')
            d = (unsigned)(x - '0');
        else if (x >= 'a' && x <= 'f')
            d = (unsigned)(x - 'a' + 10);
        else if (x >= 'A' && x <= 'F')
            d = (unsigned)(x - 'A' + 10);
        else
            return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}
static int pb_printer_pin_i(pb_core *c, int i, unsigned level) {
    uint16_t mask, value;
    if (i < 0 || (c->pins[i].mode != PB_LO && c->pins[i].mode != PB_HI) || !c->h.write_port)
        return -1;
    mask = (uint16_t)(1u << pb_defs[i].bit);
    value = level ? mask : 0;
    c->h.write_port(c->h.ctx, pb_defs[i].port, mask, value);
    c->pins[i].level = (uint8_t)level;
    c->pins[i].mode = (uint8_t)(level ? PB_HI : PB_LO);
    return 0;
}
static int pb_printer_pin(pb_core *c, const char *name, unsigned level) {
    return pb_printer_pin_i(c, pb_pin(name), level);
}
static void pb_heater_off(pb_core *c) {
    int k = pb_pin("PB11");
    c->tbp_heater_pending = 0;
    if (c->h.heater_pulse_cancel) c->h.heater_pulse_cancel(c->h.ctx);
    if (k < 0) return;
    if (c->h.set_mode &&
        c->h.set_mode(c->h.ctx, pb_defs[k].port, pb_defs[k].bit, PB_LO, 0) >= 0) {
        c->pins[k].mode = PB_LO;
        c->pins[k].level = 0;
    } else if (c->h.write_port) {
        uint16_t mask = (uint16_t)(1u << pb_defs[k].bit);
        c->h.write_port(c->h.ctx, pb_defs[k].port, mask, 0);
        c->pins[k].mode = PB_LO;
        c->pins[k].level = 0;
    }
}
static void pb_wait_us(pb_core *c, unsigned us) {
    if (us && c->h.delay_us)
        c->h.delay_us(c->h.ctx, us);
}
static int pb_all_outputs(pb_core *c, const char *const *pins, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) {
        int k = pb_pin(pins[i]);
        if (k < 0 || (c->pins[k].mode != PB_LO && c->pins[k].mode != PB_HI))
            return 0;
    }
    return 1;
}
static int pb_output_pins_available(pb_core *c, const char *const *pins, unsigned n) {
    unsigned i;
    if (!c->h.set_mode) return 0;
    for (i = 0; i < n; i++) {
        int k = pb_pin(pins[i]);
        if (k < 0 || !(pb_defs[k].caps & PB_CAP_OUT)) return 0;
    }
    return 1;
}
static void pb_drive_motor_phase(pb_core *c, unsigned phase) {
    /* This path runs from usr_timer_add's hardware-timer callback. Do not use
       hal_write here: it unconditionally disables then enables IRQs, which is
       not safe when called from an interrupt callback. Use the adapter's
       explicitly IRQ-safe fast port writer for motor phase updates. */
    unsigned i;
    for (i = 0; i < 4; i++) {
        int k = pb_pin(pb_motor_pins[i]);
        unsigned level = pb_motor_phases[phase & 7u][i];
        if (k < 0 || (c->pins[k].mode != PB_LO && c->pins[k].mode != PB_HI))
            continue;
        if (c->h.write_port_fast) {
            uint16_t mask = (uint16_t)(1u << pb_defs[k].bit);
            c->h.write_port_fast(c->h.ctx, pb_defs[k].port, mask, level ? mask : 0);
            c->pins[k].level = (uint8_t)level;
            c->pins[k].mode = (uint8_t)(level ? PB_HI : PB_LO);
        } else {
            (void)pb_printer_pin_i(c, k, level);
        }
    }
}

/* The motor stepper writes its ports from interrupt context, so a head port that shares a port
   with a motor pin must keep using the interrupt-safe write. */
static int pb_port_free_of_motor(unsigned port) {
    unsigned i;
    for (i = 0; i < 4; i++) {
        int k = pb_pin(pb_motor_pins[i]);
        if (k >= 0 && pb_defs[k].port == port)
            return 0;
    }
    return 1;
}

/* Shift one fully validated 384-bit row without repeated GPIO name lookups. */
static void pb_shift_head_row(pb_core *c, const char *hex, unsigned clock_delay_us,
                              unsigned data_delay_us) {
    uint32_t i, j;
    int pin_data = pb_pin("PB10"), pin_clk = pb_pin("PB9");
    unsigned dport = pb_defs[pin_data].port, cport = pb_defs[pin_clk].port;
    uint16_t dmask = (uint16_t)(1u << pb_defs[pin_data].bit);
    uint16_t cmask = (uint16_t)(1u << pb_defs[pin_clk].bit);
    unsigned last = 0;
    void (*wr)(void *, unsigned, uint16_t, uint16_t) = c->h.write_port;
    if (c->h.write_port_fast && pb_port_free_of_motor(dport) && pb_port_free_of_motor(cport))
        wr = c->h.write_port_fast; /* ~10x cheaper per write: no irq disable/enable */
    for (i = 0; i < 48; i++) {
        unsigned byte;
        (void)pb_hexbyte(hex + i * 2, &byte);
        for (j = 0; j < 8; j++) {
            last = (byte >> j) & 1u;
            wr(c->h.ctx, dport, dmask, last ? dmask : 0);
            pb_wait_us(c, data_delay_us); /* data setup / hold */
            wr(c->h.ctx, cport, cmask, cmask);
            pb_wait_us(c, clock_delay_us);
            wr(c->h.ctx, cport, cmask, 0);
            pb_wait_us(c, clock_delay_us);
        }
    }
    c->pins[pin_data].level = (uint8_t)last;
    c->pins[pin_data].mode = (uint8_t)(last ? PB_HI : PB_LO);
    c->pins[pin_clk].level = 0;
    c->pins[pin_clk].mode = PB_LO;
}

static int pb_hex_row_ok(const char *hex) {
    uint32_t i;
    unsigned byte;
    for (i = 0; i < 48; i++)
        if (!pb_hexbyte(hex + i * 2, &byte))
            return 0;
    return 1;
}

/* Test-only race injection points. Firmware leaves this empty; the host regression test
 * (tests/c/pb_race_test.c) defines it to run one 1 ms step tick at a chosen point, the way the
 * real timer interrupt can preempt the command task. It must never change behavior when empty. */
#ifndef PB_RACE_POINT
#define PB_RACE_POINT(c, id) ((void)0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PB_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define PB_COMPILER_BARRIER() ((void)0)
#endif

/* Start the nonblocking stepper: count includes the initial phase (N transitions = N+1). */
static void pb_step_start(pb_core *c, int reverse, uint32_t count, uint32_t gap_us,
                          uint32_t dwell_us) {
    c->tbp_feed_train_active = 0;
    c->step_segment_remaining = 0;
    c->step_seg_head = c->step_seg_count = 0;
    c->step_reverse = (uint8_t)reverse;
    c->step_remaining = count;
    c->step_gap_ms = (uint32_t)(((uint64_t)gap_us + 999u) / 1000u);
    c->step_dwell_ms = (uint32_t)(((uint64_t)dwell_us + 999u) / 1000u);
    c->step_stage = 0;
    c->step_wait_ms = c->step_dwell_ms;
    if (c->step_wait_ms == 0)
        c->step_wait_ms = 1;
    c->step_active = count != 0;
    PB_RACE_POINT(c, 1);
    c->last_feed = pb_now(c);
    if (count)
        pb_drive_motor_phase(c, c->step_phase & 7u); /* resume from the remembered phase */
    PB_RACE_POINT(c, 2);
}
/* Append one TBP feed segment to the phase train. Segment boundaries may change direction
 * or effective millisecond period; the ISR switches at the phase boundary without releasing coils. */
static int pb_step_feed_schedule_locked(pb_core *c, int reverse, uint32_t steps, uint32_t period_us,
                                        uint32_t *target_steps) {
    uint32_t period_ms = (period_us + 999u) / 1000u;
    uint16_t effective_ms = (uint16_t)(period_ms ? period_ms : 1u);
    unsigned tail;
    if (!steps) {
        *target_steps = c->step_completed;
        return 1;
    }
    PB_RACE_POINT(c, 10);
    if (c->step_active) {
        pb_step_segment *last;
        if (!c->tbp_feed_train_active || c->step_gap_ms != 0u) return 0;
        PB_RACE_POINT(c, 11);
        if (!c->step_segment_remaining && !c->step_seg_count) {
            /* The previous segment reached its final dwell; extend before the stop tick. */
            c->step_reverse = (uint8_t)reverse;
            c->step_dwell_ms = effective_ms;
            c->step_segment_remaining = steps;
        } else if (!c->step_seg_count && c->step_reverse == (uint8_t)reverse &&
                   c->step_dwell_ms == effective_ms) {
            c->step_segment_remaining += steps;
        } else {
            tail = ((unsigned)c->step_seg_head + c->step_seg_count) % PB_TBP_QUEUE_DEPTH;
            last = c->step_seg_count ? &c->step_segments[tail ? tail - 1u : PB_TBP_QUEUE_DEPTH - 1u] : 0;
            if (last && last->reverse == (uint8_t)reverse && last->period_ms == effective_ms) {
                last->transitions += steps;
            } else {
                if (c->step_seg_count >= PB_TBP_QUEUE_DEPTH) return 0;
                c->step_segments[tail].transitions = steps;
                c->step_segments[tail].reverse = (uint8_t)reverse;
                c->step_segments[tail].period_ms = effective_ms;
                c->step_seg_count++;
            }
        }
        c->step_remaining += steps;
        PB_RACE_POINT(c, 12);
        c->step_scheduled_total += steps;
        *target_steps = c->step_scheduled_total;
        return 1;
    }
    pb_step_start(c, reverse, steps + 1u, 0u, period_us);
    PB_RACE_POINT(c, 3);
    c->tbp_feed_train_active = 1;
    PB_RACE_POINT(c, 4);
    c->step_segment_remaining = steps;
    PB_RACE_POINT(c, 5);
    c->step_scheduled_total = c->step_completed + steps;
    *target_steps = c->step_scheduled_total;
    return 1;
}

/* The 1 ms timer ISR (pb_step_tick) mutates step_active/step_remaining/step_completed and the
 * segment queue, and can preempt the command task anywhere. While step_lock is set the ISR skips
 * its tick (the motor just waits one more millisecond), so the start/extend below sees one
 * consistent train: the base step count, the train state and the target are all fixed before the
 * ISR runs again. Without this, a tick between step_active=1 and the target computation made the
 * target one higher than the train could ever reach, and a tick that ended the train in the middle
 * of an extend left the extended target unreachable (bug 2026-11-03). */
static int pb_step_feed_schedule(pb_core *c, int reverse, uint32_t steps, uint32_t period_us,
                                uint32_t *target_steps) {
    int ok;
    c->step_lock = 1;
    PB_COMPILER_BARRIER();
    ok = pb_step_feed_schedule_locked(c, reverse, steps, period_us, target_steps);
    PB_COMPILER_BARRIER();
    c->step_lock = 0;
    return ok;
}

/* One gated heater pulse for the legacy PHEAT/PEVENT commands. Same gates as a TBP ROW: real
 * temperature/battery/paper readings, the start temperature cutoff, the per-pulse maximum and the
 * shared duty budget. Returns NULL after the pulse, or the legacy error code and no heat. */
static const char *pb_legacy_heat(pb_core *c, unsigned us) {
    pb_sensor_snapshot snap;
    uint8_t code, arg;
    pb_gate g;
    uint32_t now = pb_now(c);
    if (!us) return 0;
    if (us > PB_TBP_SLICE_MAX_US) return "range";
    pb_sensors_sample(c, &snap);
    g = pb_heat_gate(&snap, PB_TUNE_TEMP_CUTOFF_C, 0, &code, &arg);
    if (g == PB_GATE_HOT) return "hot";
    if (g == PB_GATE_FAIL) return code == 0x10u ? "nopaper" : code == 0x11u ? "lowbat" : "sensor";
    if (PB_TBP_DUTY_WINDOW_MS && (uint32_t)(now - c->tbp_heat_window_start) >= PB_TBP_DUTY_WINDOW_MS) {
        c->tbp_heat_window_start = now;
        c->tbp_heat_used_us = 0;
    }
    if (us > PB_TBP_DUTY_MAX_US || c->tbp_heat_used_us > PB_TBP_DUTY_MAX_US - us) return "duty";
    c->tbp_heat_used_us += us;
    if (pb_printer_pin(c, "PB11", 1) < 0) return "mod";
    pb_wait_us(c, us);
    pb_heater_off(c);
    return 0;
}

static void pb_printer_command(pb_core *c, char **v, int n) {
    static const char *const headpins[] = {"PB10", "PB9", "PB8", "PB11"};
    uint32_t a, b, count;
    if (!c->h.delay_us) {
        pb_error(c, "bsy", "delay");
        return;
    }
    if (pb_eq(v[0], "PHEAD")) {
        if (n != 4 || strlen(v[1]) != 96 || !pb_u32(v[2], &a) || !pb_u32(v[3], &b) ||
            a > 10u || b > 10u) {
            pb_error(c, "arg", "PHEAD");
            return;
        }
        if (!pb_hex_row_ok(v[1])) {
            pb_error(c, "hex", "PHEAD");
            return;
        }
        if (pb_tbp_used(c) || c->step_active) {
            pb_error(c, "bsy", "PHEAD");
            return;
        }
        if (!pb_all_outputs(c, headpins, 4) || !c->h.write_port) {
            pb_error(c, "mod", "PB10");
            return;
        }
        pb_shift_head_row(c, v[1], (unsigned)a, (unsigned)b);
        pb_reply(c, "ok bits=384");
        c->last_feed = pb_now(c);
        return;
    }
    if (pb_eq(v[0], "PLATCH")) {
        if (n != 2 || !pb_u32(v[1], &a) || a > 1000u) {
            pb_error(c, "arg", "PLATCH");
            return;
        }
        if (pb_tbp_used(c) || c->step_active) {
            pb_error(c, "bsy", "PLATCH");
            return;
        }
        if (!pb_all_outputs(c, headpins, 4)) {
            pb_error(c, "mod", "PB8");
            return;
        }
        (void)pb_printer_pin(c, "PB8", 0);
        pb_wait_us(c, (unsigned)a);
        (void)pb_printer_pin(c, "PB8", 1);
        pb_reply(c, "ok");
        c->last_feed = pb_now(c);
        return;
    }
    if (pb_eq(v[0], "PHEAT")) {
        const char *why;
        if (n != 2 || !pb_u32(v[1], &a) || a > PB_TUNE_HEATER_REQUEST_MAX_US) {
            pb_error(c, "arg", "PHEAT");
            return;
        }
        if (pb_tbp_used(c)) {
            pb_error(c, "bsy", "PHEAT");
            return;
        }
        if (!pb_all_outputs(c, headpins, 4)) {
            pb_error(c, "mod", "PB11");
            return;
        }
        why = pb_legacy_heat(c, (unsigned)a);
        if (why) {
            pb_heater_off(c);
            pb_error(c, why, "PHEAT");
            return;
        }
        pb_reply(c, "ok");
        c->last_feed = pb_now(c);
        return;
    }
    if (pb_eq(v[0], "PSTEP")) {
        if (n != 5 || (!pb_eq(v[1], "FWD") && !pb_eq(v[1], "REV")) || !pb_u32(v[2], &count) ||
            !pb_u32(v[3], &a) || !pb_u32(v[4], &b) || count > 10000u || a > 1000000u ||
            b < PB_TBP_MIN_PERIOD_US || b > 65535u) {
            pb_error(c, "arg", "PSTEP");
            return;
        }
        if (pb_tbp_used(c) || c->step_active) {
            pb_error(c, "bsy", "PSTEP");
            return;
        }
        if (!pb_all_outputs(c, pb_motor_pins, 4)) pb_motor_outputs_init(c);
        if (!pb_all_outputs(c, pb_motor_pins, 4)) {
            pb_motor_outputs_release(c);
            pb_error(c, "mod", "PA9");
            return;
        }
        pb_step_start(c, pb_eq(v[1], "REV"), count, a, b);
        pb_reply(c, count ? "ok started" : "ok halfsteps");
        return;
    }
    if (pb_eq(v[0], "PEVENT")) {
        /* Legacy event: heat goes through the same gates as a TBP ROW (pb_legacy_heat). */
        uint32_t latch_us, heat_us;
        if (n != 7 || strlen(v[1]) != 96 || !pb_u32(v[2], &a) || !pb_u32(v[3], &b) ||
            !pb_u32(v[4], &latch_us) || !pb_u32(v[5], &heat_us) || !pb_u32(v[6], &count) ||
            a > PB_TUNE_HEAD_TIMING_MAX_US || b > PB_TUNE_HEAD_TIMING_MAX_US ||
            latch_us > PB_TUNE_HEAD_TIMING_MAX_US || heat_us > PB_TUNE_HEATER_REQUEST_MAX_US || count > 8u) {
            pb_error(c, "arg", "PEVENT");
            return;
        }
        if (!pb_hex_row_ok(v[1])) {
            pb_error(c, "hex", "PEVENT");
            return;
        }
        if (pb_tbp_used(c) || c->step_active) {
            pb_error(c, "bsy", "PEVENT");
            return;
        }
        if (!pb_all_outputs(c, headpins, 4) || !c->h.write_port) {
            pb_error(c, "mod", "PB10");
            return;
        }
        if (count && !pb_all_outputs(c, pb_motor_pins, 4)) pb_motor_outputs_init(c);
        if (count && !pb_all_outputs(c, pb_motor_pins, 4)) {
            pb_motor_outputs_release(c);
            pb_error(c, "mod", "PA9");
            return;
        }
        if (heat_us) {
            /* Check the gates before any head output changes; a refused event does nothing. */
            pb_sensor_snapshot snap;
            uint8_t code, arg;
            pb_gate g;
            pb_sensors_sample(c, &snap);
            g = pb_heat_gate(&snap, PB_TUNE_TEMP_CUTOFF_C, 0, &code, &arg);
            if (g != PB_GATE_OK) {
                pb_heater_off(c);
                pb_error(c, g == PB_GATE_HOT ? "hot" : code == 0x10u ? "nopaper" : code == 0x11u ? "lowbat" : "sensor", "PEVENT");
                return;
            }
        }
        pb_shift_head_row(c, v[1], (unsigned)a, (unsigned)b);
        (void)pb_printer_pin(c, "PB8", 0);
        pb_wait_us(c, (unsigned)latch_us);
        (void)pb_printer_pin(c, "PB8", 1);
        if (heat_us) {
            const char *why = pb_legacy_heat(c, (unsigned)heat_us); /* re-checks the gates right before heat */
            if (why) {
                pb_heater_off(c);
                pb_error(c, why, "PEVENT");
                return;
            }
        }
        pb_heater_off(c);
        if (count) pb_step_start(c, 0, count + 1u, 0, PB_TBP_MIN_PERIOD_US);
        c->last_feed = pb_now(c);
        pb_reply(c, "ok event");
        return;
    }
    if (pb_eq(v[0], "PSTEPSTAT")) {
        if (n != 1) {
            pb_error(c, "arg", "PSTEPSTAT");
            return;
        }
        pb_reply(c, c->step_active ? "ok busy" : "ok done");
        c->last_feed = pb_now(c);
        return;
    }
    if (pb_eq(v[0], "PTIME")) {
        /* PTIME <src> [ms]: blocking timer check. The host timestamps the "ptime begin" and "ok ptime" lines
         * and compares the wall time with what the timer claims it counted. Refused while anything else
         * could be using a timer or the head, because the command task is stalled for the whole run. */
        unsigned src = 0;
        uint32_t ms = PB_TIME_MS_DEFAULT;
        int rc;
        while (src < PB_TIME_NSRC && (n < 2 || !pb_eq(v[1], pb_time_srcs[src])))
            src++;
        if (n < 2 || n > 3 || src >= PB_TIME_NSRC || (n == 3 && !pb_u32(v[2], &ms)) ||
            ms < PB_TIME_MS_MIN || ms > PB_TIME_MS_MAX) {
            pb_error(c, "arg", "PTIME");
            return;
        }
        if (!c->h.time_test) {
            pb_error(c, "nosup", "PTIME");
            return;
        }
        if (pb_tbp_used(c) || c->step_active || c->step_timer_id ||
            (c->h.heater_pulse_active && c->h.heater_pulse_active(c->h.ctx))) {
            pb_error(c, "bsy", "PTIME");
            return;
        }
        rc = c->h.time_test(c->h.ctx, src, (unsigned)ms);
        if (rc == 1)
            pb_error(c, "bsy", "PTIME");
        else if (rc)
            pb_error(c, "fail", "PTIME");
        c->last_feed = pb_now(c);
        return;
    }
    if (pb_eq(v[0], "PHDIAG")) {
        char b[120] = "ok hdiag starts=";
        size_t z = strlen(b);
        if (n != 1) {
            pb_error(c, "arg", "PHDIAG");
            return;
        }
        (void)pb_num(b, sizeof b, &z, c->heater_pulse_starts);
        (void)pb_append(b, sizeof b, &z, " isr=");
        (void)pb_num(b, sizeof b, &z, c->heater_isr_count);
        (void)pb_append(b, sizeof b, &z, " poll=");
        (void)pb_num(b, sizeof b, &z, c->heater_poll_ends);
        (void)pb_append(b, sizeof b, &z, " over=");
        (void)pb_num(b, sizeof b, &z, c->heater_overruns);
        (void)pb_append(b, sizeof b, &z, " cnt=");
        (void)pb_num(b, sizeof b, &z, c->heater_last_cnt);
        (void)pb_append(b, sizeof b, &z, " con=");
        (void)pb_num(b, sizeof b, &z, c->heater_last_con);
        pb_reply(c, b);
        return;
    }
    if (pb_eq(v[0], "PDIAG")) {
        char b[160] = "ok diag step=";
        size_t z = strlen(b);
        if (n != 1) {
            pb_error(c, "arg", "PDIAG");
            return;
        }
        (void)pb_num(b, sizeof b, &z, c->step_active);
        (void)pb_append(b, sizeof b, &z, " phase=");
        (void)pb_num(b, sizeof b, &z, c->step_phase);
        (void)pb_append(b, sizeof b, &z, " done=");
        (void)pb_num(b, sizeof b, &z, c->step_completed);
        (void)pb_append(b, sizeof b, &z, " rem=");
        (void)pb_num(b, sizeof b, &z, c->step_remaining);
        (void)pb_append(b, sizeof b, &z, " timer=");
        (void)pb_num(b, sizeof b, &z, c->step_timer_id);
        (void)pb_append(b, sizeof b, &z, " ticks=");
        (void)pb_num(b, sizeof b, &z, c->step_timer_callbacks);
        (void)pb_append(b, sizeof b, &z, " regfail=");
        (void)pb_num(b, sizeof b, &z, c->step_timer_reg_failures);
        (void)pb_append(b, sizeof b, &z, " wiggle=");
        (void)pb_num(b, sizeof b, &z, c->wiggle_stage);
        (void)pb_append(b, sizeof b, &z, " request=");
        (void)pb_num(b, sizeof b, &z, c->wiggle_request);
        (void)pb_append(b, sizeof b, &z, " rowpaper=");
        (void)pb_num(b, sizeof b, &z, c->tbp_row_snap.paper_valid);
        (void)pb_append(b, sizeof b, &z, "/");
        (void)pb_num(b, sizeof b, &z, c->tbp_row_snap.paper_present);
        (void)pb_append(b, sizeof b, &z, " power=");
        (void)pb_num(b, sizeof b, &z, c->power_ready);
        (void)pb_append(b, sizeof b, &z, " abort=");
        (void)pb_num(b, sizeof b, &z, c->emergency_abort_requested);
        pb_reply(c, b);
        return;
    }
}

static inline void pb_step_tick(pb_core *c) {
    unsigned phase;
    if (!c->step_active || c->step_lock)
        return;
    c->last_feed = pb_now(c);
    if (c->tbp_feed_train_active) {
        if (c->step_wait_ms > 1u) {
            c->step_wait_ms--;
            return;
        }
        c->step_wait_ms = 0;
        if (!c->step_stage) {
            c->step_stage = 1u;
            if (c->step_gap_ms) {
                c->step_wait_ms = c->step_gap_ms;
                return;
            }
        } else c->step_stage = 0;
        if (--c->step_remaining == 0u) {
            c->step_active = 0;
            c->step_done_pending = 1;
            c->tbp_feed_train_active = 0;
            c->step_segment_remaining = 0;
            c->step_seg_head = c->step_seg_count = 0;
            return;
        }
        phase = c->step_reverse ? ((c->step_phase + 7u) & 7u) : ((c->step_phase + 1u) & 7u);
        c->step_phase = (uint8_t)phase;
        c->step_completed++;
        pb_drive_motor_phase(c, phase);
        if (c->step_segment_remaining) c->step_segment_remaining--;
        if (!c->step_segment_remaining && c->step_seg_count) {
            pb_step_segment *next = &c->step_segments[c->step_seg_head];
            c->step_reverse = next->reverse;
            c->step_dwell_ms = next->period_ms;
            c->step_segment_remaining = next->transitions;
            c->step_seg_head = (uint8_t)((c->step_seg_head + 1u) % PB_TBP_QUEUE_DEPTH);
            c->step_seg_count--;
        }
        c->step_stage = 0;
        c->step_wait_ms = c->step_dwell_ms ? c->step_dwell_ms : 1u;
        return;
    }
    if (c->step_wait_ms > 1) {
        c->step_wait_ms--;
        return;
    }
    c->step_wait_ms = 0;
    if (!c->step_stage) {
        c->step_stage = 1;
        if (c->step_gap_ms) {
            c->step_wait_ms = c->step_gap_ms;
            return;
        }
        /* Zero gap: advance phase in this same 1 ms tick. */
    } else {
        c->step_stage = 0;
    }
    if (--c->step_remaining == 0) {
        c->step_active = 0;
        c->step_done_pending = 1;
        return;
    }
    phase = c->step_reverse ? ((c->step_phase + 7u) & 7u) : ((c->step_phase + 1u) & 7u);
    c->step_phase = (uint8_t)phase;
    c->step_completed++;
    pb_drive_motor_phase(c, phase);
    c->step_stage = 0;
    c->step_wait_ms = c->step_dwell_ms;
    if (!c->step_wait_ms)
        c->step_wait_ms = 1;
}

#endif
