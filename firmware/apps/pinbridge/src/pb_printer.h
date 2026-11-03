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
    for (unsigned i = 0; i < 4; i++)
        (void)pb_printer_pin(c, pb_motor_pins[i], pb_motor_phases[phase][i]);
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

/* Start the nonblocking stepper: count includes the initial phase (N transitions = N+1). */
static void pb_step_start(pb_core *c, int reverse, uint32_t count, uint32_t gap_us,
                          uint32_t dwell_us) {
    c->step_reverse = (uint8_t)reverse;
    c->step_remaining = count;
    c->step_gap_ms = (uint32_t)(((uint64_t)gap_us + 999u) / 1000u);
    c->step_dwell_ms = (uint32_t)(((uint64_t)dwell_us + 999u) / 1000u);
    c->step_stage = 0;
    c->step_wait_ms = c->step_dwell_ms;
    if (c->step_wait_ms == 0)
        c->step_wait_ms = 1;
    c->step_active = count != 0;
    c->last_feed = pb_now(c);
    if (count)
        pb_drive_motor_phase(c, c->step_phase & 7u); /* resume from the remembered phase */
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
}

static inline void pb_step_tick(pb_core *c) {
    unsigned phase;
    if (!c->step_active)
        return;
    c->last_feed = pb_now(c);
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
    pb_drive_motor_phase(c, phase);
    c->step_stage = 0;
    c->step_wait_ms = c->step_dwell_ms;
    if (!c->step_wait_ms)
        c->step_wait_ms = 1;
}

#endif
