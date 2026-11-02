/* Internal to pb_core.h; shares its core state and reply helpers. */
#ifndef PB_PRINTER_H
#define PB_PRINTER_H

/* Printer-specific commands and the nonblocking motor stepper. */
static const char *const pb_motor_pins[4] = {"PA9", "PA10", "PC3", "PC2"};
/* Official JieLi AC6956C half-step sequence on PA9, PA10, PC3, PC2.
 Static firmware trace only; pin polarity and motor wiring remain unverified. */
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
static void pb_drive_motor_phase(pb_core *c, unsigned phase) {
    for (unsigned i = 0; i < 4; i++)
        (void)pb_printer_pin(c, pb_motor_pins[i], pb_motor_phases[phase][i]);
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
    for (i = 0; i < 48; i++) {
        unsigned byte;
        (void)pb_hexbyte(hex + i * 2, &byte);
        for (j = 0; j < 8; j++) {
            last = (byte >> j) & 1u;
            c->h.write_port(c->h.ctx, dport, dmask, last ? dmask : 0);
            pb_wait_us(c, data_delay_us); /* data setup / hold */
            c->h.write_port(c->h.ctx, cport, cmask, cmask);
            pb_wait_us(c, clock_delay_us);
            c->h.write_port(c->h.ctx, cport, cmask, 0);
            pb_wait_us(c, clock_delay_us);
        }
    }
    c->pins[pin_data].level = (uint8_t)last;
    c->pins[pin_data].mode = (uint8_t)(last ? PB_HI : PB_LO);
    c->pins[pin_clk].level = 0;
    c->pins[pin_clk].mode = PB_LO;
}

static void pb_printer_command(pb_core *c, char **v, int n) {
    static const char *const headpins[] = {"PB10", "PB9", "PB8", "PB11"};
    uint32_t a, b, count, i;
    char *hex;
    if (!c->h.delay_us) {
        pb_error(c, "bsy", "delay");
        return;
    }
    if (pb_eq(v[0], "PHEAD")) {
        if (n != 4 || strlen(v[1]) != 96 || !pb_u32(v[2], &a) || !pb_u32(v[3], &b) ||
            !pb_all_outputs(c, headpins, 4)) {
            pb_error(c, "arg", "PHEAD");
            return;
        }
        hex = v[1];
        for (i = 0; i < 48; i++) {
            unsigned byte;
            if (!pb_hexbyte(hex + i * 2, &byte)) {
                pb_error(c, "hex", "PHEAD");
                return;
            }
        }
        if (!c->h.write_port) {
            pb_error(c, "mod", "PB10");
            return;
        }
        pb_shift_head_row(c, hex, (unsigned)a, (unsigned)b);
        pb_reply(c, "ok bits=384");
        c->last_feed = pb_now(c);
        return;
    }
    if (pb_eq(v[0], "PLATCH")) {
        if (n != 2 || !pb_u32(v[1], &a) || !pb_all_outputs(c, headpins, 4)) {
            pb_error(c, "arg", "PLATCH");
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
        /* Experimental single pulse. Duration policy belongs to the host-side demo. */
        if (n != 2 || !pb_u32(v[1], &a) || !pb_all_outputs(c, headpins, 4)) {
            pb_error(c, "arg", "PHEAT");
            return;
        }
        (void)pb_printer_pin(c, "PB11", 1);
        pb_wait_us(c, (unsigned)a);
        (void)pb_printer_pin(c, "PB11", 0);
        pb_reply(c, "ok");
        c->last_feed = pb_now(c);
        return;
    }
    if (pb_eq(v[0], "PSTEP")) {
        unsigned phase;
        if (n != 5 || (!pb_eq(v[1], "FWD") && !pb_eq(v[1], "REV")) || !pb_u32(v[2], &count) ||
            !pb_u32(v[3], &a) || !pb_u32(v[4], &b) || !pb_all_outputs(c, pb_motor_pins, 4) ||
            c->step_active) {
            pb_error(c, "arg", "PSTEP");
            return;
        }
        /* Nonblocking 1 ms scheduler: USB command handling and watchdog polling
           continue during motion. Host-side code owns duration/range policy. */
        c->step_reverse = (uint8_t)pb_eq(v[1], "REV");
        c->step_remaining = count;
        c->step_gap_ms = (uint32_t)(((uint64_t)a + 999u) / 1000u);
        c->step_dwell_ms = (uint32_t)(((uint64_t)b + 999u) / 1000u);
        c->step_stage = 0;
        c->step_wait_ms = c->step_dwell_ms;
        if (c->step_wait_ms == 0)
            c->step_wait_ms = 1;
        c->step_active = count != 0;
        c->last_feed = pb_now(c);
        if (count) {
            phase = c->step_phase & 7u; /* resume from the remembered phase */
            pb_drive_motor_phase(c, phase);
            pb_reply(c, "ok started");
        } else
            pb_reply(c, "ok halfsteps");
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
