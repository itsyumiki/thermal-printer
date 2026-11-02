/* Internal to pb_core.h; shares its core state and reply helpers. */
#ifndef PB_COMMANDS_H
#define PB_COMMANDS_H

/* Mode, GPIO, ADC and listener operations over the portable HAL. */
static void pb_control_command(pb_core *c, char **v, int n, uint32_t now) {
    char *b = c->reply;
    size_t z = 0;
    if (pb_eq(v[0], "HEL")) {
        if (n != 1) {
            pb_error(c, "arg", n > 1 ? v[1] : "");
            return;
        }
        pb_append(b, sizeof(c->reply), &z, "ok jlpin 2 " PB_CFG_HASH_STR " up=");
        pb_num(b, sizeof(c->reply), &z, now);
        pb_reply(c, b);
        c->last_feed = now;
        return;
    }
    if (pb_eq(v[0], "DBG")) {
        if (n != 2 || (!pb_eq(v[1], "on") && !pb_eq(v[1], "off"))) {
            pb_error(c, "arg", n > 1 ? v[1] : "");
            return;
        }
        c->dbg = pb_eq(v[1], "on");
        pb_reply(c, "ok");
        c->last_feed = now;
        return;
    }
    if (pb_eq(v[0], "WDG")) {
        uint32_t timeout;
        if (n != 2) {
            pb_error(c, "arg", "");
            return;
        }
        if (!pb_u32(v[1], &timeout) || (timeout && (timeout < 100 || timeout > 3600000))) {
            pb_error(c, "arg", v[1]);
            return;
        }
        c->wdg = timeout;
        pb_reply(c, "ok");
        c->last_feed = now;
        return;
    }
    if (pb_eq(v[0], "RST")) {
        if (n != 1) {
            pb_error(c, "arg", v[1]);
            return;
        }
        pb_safe_state(c);
        pb_reply(c, "ok");
        c->last_feed = now;
        return;
    }
}

static void pb_cmd_mode(pb_core *c, char **v, int n, uint32_t now) {
    int i, j;
    if (n != 3) {
        pb_error(c, "arg", n > 1 ? v[1] : "");
        return;
    }
    i = pb_pin(v[1]);
    if (i < 0) {
        pb_error(c, "pin", v[1]);
        return;
    }
    int m = -1;
    for (j = 1; j < 9; j++)
        if (pb_eq(v[2], pb_modes[j]))
            m = j;
    if (m < 0) {
        pb_error(c, "arg", v[2]);
        return;
    }
    unsigned cap = 0;
    switch (m) {
    case PB_IN:
        cap = PB_CAP_IN;
        break;
    case PB_UP:
        cap = PB_CAP_PU;
        break;
    case PB_DN:
        cap = PB_CAP_PD;
        break;
    case PB_LO:
    case PB_HI:
        cap = PB_CAP_OUT;
        break;
    case PB_ADC:
        cap = PB_CAP_ADC;
        break;
    case PB_PWM:
        cap = PB_CAP_PWM;
        break;
    }
    if (cap && !(pb_defs[i].caps & cap)) {
        pb_error(c, "cap", v[1]);
        return;
    }
    if (pb_eq(pb_defs[i].name, "PB11") && m != PB_LO) {
        pb_error(c, "bsy", "PB11");
        return;
    }
    if (m == PB_PWM && !c->h.pwm_set) {
        pb_error(c, "bsy", v[1]);
        return;
    }
    if (m == PB_ADC && c->h.adc_read && c->h.adc_read(c->h.ctx, pb_defs[i].adc, 0, 0) < 0) {
        pb_error(c, "bsy", v[1]);
        return;
    }
    if (c->h.set_mode &&
        c->h.set_mode(c->h.ctx, pb_defs[i].port, pb_defs[i].bit, (unsigned)m, m == PB_HI) < 0) {
        pb_error(c, "bsy", v[1]);
        return;
    }
    c->pins[i].mode = (uint8_t)m;
    c->pins[i].level = (m == PB_HI);
    c->pins[i].listen = 0;
    pb_reply(c, "ok");
    c->last_feed = now;
    return;
}

static void pb_cmd_set(pb_core *c, char **v, int n, uint32_t now) {
    int i, *p = c->scratch_p;
    if (n < 2) {
        pb_error(c, "arg", "");
        return;
    }
    uint16_t masks[4] = {0}, vals[4] = {0};
    uint16_t seen = 0;
    for (i = 1; i < n; i++) {
        char *eq = strchr(v[i], '=');
        if (!eq) {
            pb_error(c, "arg", v[i]);
            return;
        }
        *eq++ = 0;
        p[i - 1] = pb_pin(v[i]);
        if (p[i - 1] < 0) {
            pb_error(c, "pin", v[i]);
            return;
        }
        if (!pb_eq(eq, "0") && !pb_eq(eq, "1")) {
            pb_error(c, "arg", eq);
            return;
        }
        if (pb_eq(pb_defs[p[i - 1]].name, "PB11") && *eq == '1') {
            pb_error(c, "bsy", "PB11");
            return;
        }
        uint16_t pin_bit = (uint16_t)(1u << p[i - 1]);
        if (seen & pin_bit) {
            pb_error(c, "arg", v[i]);
            return;
        }
        seen |= pin_bit;
        if (c->pins[p[i - 1]].mode != PB_LO && c->pins[p[i - 1]].mode != PB_HI) {
            pb_error(c, "mod", v[i]);
            return;
        }
        masks[pb_defs[p[i - 1]].port] |= (1u << pb_defs[p[i - 1]].bit);
        if (*eq == '1')
            vals[pb_defs[p[i - 1]].port] |= (1u << pb_defs[p[i - 1]].bit);
    }
    for (i = 0; i < 4; i++) {
        if (masks[i] && c->h.write_port)
            c->h.write_port(c->h.ctx, i, (uint16_t)masks[i], (uint16_t)vals[i]);
    }
    for (i = 1; i < n; i++) {
        int k = p[i - 1];
        c->pins[k].level = (vals[pb_defs[k].port] >> pb_defs[k].bit) & 1u;
        c->pins[k].mode = c->pins[k].level ? PB_HI : PB_LO;
    }
    pb_reply(c, "ok");
    c->last_feed = now;
    return;
}

static void pb_cmd_read(pb_core *c, char **v, int n, uint32_t now) {
    int i, *p = c->scratch_p;
    char *b = c->reply;
    size_t z = 0;
    int sta = pb_eq(v[0], "STA");
    if (n == 1) {
        for (i = 0; i < PB_NPINS; i++)
            p[i] = i;
        n = PB_NPINS;
    } else {
        for (i = 1; i < n; i++) {
            p[i - 1] = pb_pin(v[i]);
            if (p[i - 1] < 0) {
                pb_error(c, "pin", v[i]);
                return;
            }
        }
        n--;
    }
    /* Preflight reply length and pin modes before emitting any part of the response. */
    {
        size_t need = 2;
        for (i = 0; i < n; i++) {
            int k = p[i];
            if (sta && c->pins[k].mode == PB_OFF)
                continue;
            if (!sta && (c->pins[k].mode < PB_IN || c->pins[k].mode > PB_HI)) {
                pb_error(c, "mod", pb_defs[k].name);
                return;
            }
            need += 1 + strlen(pb_defs[k].name) + 1 + (sta ? strlen(pb_modes[c->pins[k].mode]) : 1);
        }
        if (need > 127) {
            pb_error(c, "len", "");
            return;
        }
    }
    pb_append(b, sizeof(c->reply), &z, "ok");
    for (i = 0; i < n; i++) {
        int k = p[i];
        if (sta && c->pins[k].mode == PB_OFF)
            continue;
        pb_append(b, sizeof(c->reply), &z, " ");
        pb_append(b, sizeof(c->reply), &z, pb_defs[k].name);
        pb_append(b, sizeof(c->reply), &z, "=");
        if (sta) {
            pb_append(b, sizeof(c->reply), &z, pb_modes[c->pins[k].mode]);
        } else {
            unsigned level = c->pins[k].level;
            /* Input reads must sample the physical pad, not the output shadow.
             * The shadow is only meaningful for output modes. */
            if (c->pins[k].mode == PB_IN || c->pins[k].mode == PB_UP || c->pins[k].mode == PB_DN) {
                if (!c->h.read_port) {
                    pb_error(c, "bsy", pb_defs[k].name);
                    return;
                }
                level = pb_sample_pin(c, k);
                c->pins[k].last = (uint8_t)level;
            }
            pb_append(b, sizeof(c->reply), &z, level ? "1" : "0");
        }
    }
    pb_reply(c, b);
    c->last_feed = now;
    return;
}

static void pb_cmd_adc(pb_core *c, char **v, int n, uint32_t now) {
    int i, *p = c->scratch_p;
    char *b = c->reply;
    size_t z = 0;
    unsigned raw, mv;
    if (n < 2) {
        pb_error(c, "arg", "");
        return;
    }
    pb_append(b, sizeof(c->reply), &z, "ok");
    for (i = 1; i < n; i++) {
        if (pb_eq(v[i], "vbat")) {
            p[i - 1] = -2;
        } else {
            p[i - 1] = pb_pin(v[i]);
            if (p[i - 1] < 0) {
                pb_error(c, "pin", v[i]);
                return;
            }
            if (c->pins[p[i - 1]].mode != PB_ADC) {
                pb_error(c, "mod", v[i]);
                return;
            }
        }
    }
    for (i = 1; i < n; i++) {
        int ch = p[i - 1] == -2 ? -2 : pb_defs[p[i - 1]].adc;
        if (!c->h.adc_read || c->h.adc_read(c->h.ctx, ch, &raw, &mv)) {
            pb_error(c, "bsy", v[i]);
            return;
        }
        pb_append(b, sizeof(c->reply), &z, " ");
        pb_append(b, sizeof(c->reply), &z, p[i - 1] == -2 ? "vbat" : pb_defs[p[i - 1]].name);
        pb_append(b, sizeof(c->reply), &z, "=");
        pb_num(b, sizeof(c->reply), &z, raw);
        pb_append(b, sizeof(c->reply), &z, "/");
        pb_num(b, sizeof(c->reply), &z, mv);
    }
    pb_reply(c, b);
    c->last_feed = now;
    return;
}

static void pb_cmd_listen(pb_core *c, char **v, int n, uint32_t now) {
    int i, *p = c->scratch_p;
    int off = n > 1 && pb_eq(v[1], "off"), start = off ? 2 : 1;
    if (off && n == 2) {
        for (i = 0; i < PB_NPINS; i++)
            c->pins[i].listen = 0;
    } else {
        if (start >= n) {
            pb_error(c, "arg", "");
            return;
        }
        for (i = start; i < n; i++) {
            p[i - start] = pb_pin(v[i]);
            if (p[i - start] < 0) {
                pb_error(c, "pin", v[i]);
                return;
            }
            int k = p[i - start], m = c->pins[k].mode;
            if (!off && m != PB_IN && m != PB_UP && m != PB_DN) {
                pb_error(c, "mod", v[i]);
                return;
            }
        }
        for (i = start; i < n; i++) {
            int k = p[i - start];
            c->pins[k].listen = !off;
            if (!off && c->h.read_port)
                c->pins[k].last = pb_sample_pin(c, k);
        }
    }
    pb_reply(c, "ok");
    c->last_feed = now;
    return;
}

#endif
