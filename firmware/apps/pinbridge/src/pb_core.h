/* pinbridge portable protocol core. GPL-3.0-or-later. Header-only for the fixed
 * SDK build. */
#ifndef PB_CORE_H
#define PB_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define PB_CAP_IN 1u
#define PB_CAP_OUT 2u
#define PB_CAP_PU 4u
#define PB_CAP_PD 8u
#define PB_CAP_ADC 16u
#define PB_CAP_PWM 32u
#define PB_NOADC (-1)
#ifndef PB_PINS_HEADER
#define PB_PINS_HEADER "pb_pins.h"
#endif
#include PB_PINS_HEADER
typedef struct {
    const char *name;
    uint8_t port, bit;
    uint16_t caps;
    int8_t adc;
    uint8_t pwm;
} pb_pin_def;
#define PB_DEF(n, p, b, c, a, w) {n, p, b, c, a, w},
static const pb_pin_def pb_defs[] = {PB_PIN_LIST(PB_DEF)};
#undef PB_DEF
typedef enum { PB_OFF, PB_HIZ, PB_IN, PB_UP, PB_DN, PB_LO, PB_HI, PB_ADC, PB_PWM } pb_mode;
typedef struct {
    uint8_t mode, level, listen, last;
} pb_pin_state;
#define PB_MAX_PINS 16u
typedef struct {
    void *ctx;
    int (*set_mode)(void *, unsigned, unsigned, unsigned, unsigned);
    void (*write_port)(void *, unsigned, uint16_t, uint16_t);
    uint16_t (*read_port)(void *, unsigned);
    int (*adc_read)(void *, int, unsigned *, unsigned *);
    int (*pwm_set)(void *, unsigned, unsigned, unsigned);
    uint32_t (*millis)(void *);
    int (*tx_write)(void *, const char *, size_t);
    void (*delay_us)(void *, unsigned);
} pb_hal;
typedef struct {
    pb_hal h;
    pb_pin_state pins[PB_NPINS];
    char line[129];
    unsigned llen, discard, dbg, wdg;
    uint32_t last_feed;
    unsigned pollport;
    char reply[128];
    size_t rlen;
    volatile uint8_t step_active, step_reverse, step_stage, step_done_pending;
    volatile uint8_t
        step_phase; /* persistent half-step phase 0..7; survives PSTEP commands, reset by pb_init */
    volatile uint32_t step_remaining, step_wait_ms;
    uint32_t step_gap_ms, step_dwell_ms;
    int scratch_p[PB_MAX_PINS];
    char *scratch_v[17];
} pb_core;
static const char *pb_modes[] = {"off", "hiz", "in", "up", "dn", "lo", "hi", "adc", "pwm"};
static uint32_t pb_now(pb_core *c) {
    return c->h.millis ? c->h.millis(c->h.ctx) : 0;
}
static int pb_eq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return 0;
    }
    return !*a && !*b;
}
static int pb_token(char *s, char **v, int max) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        if (n == max)
            return max + 1;
        v[n++] = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = 0;
    }
    return n;
}
static int pb_pin(const char *s) {
    unsigned i;
    for (i = 0; i < PB_NPINS; i++)
        if (pb_eq(s, pb_defs[i].name))
            return (int)i;
    return -1;
}
static void pb_emit(pb_core *c, const char *s) {
    if (c->h.tx_write)
        c->h.tx_write(c->h.ctx, s, strlen(s));
}
static void pb_reply(pb_core *c, const char *s) {
    pb_emit(c, s);
    pb_emit(c, "\n");
}
static void pb_error(pb_core *c, const char *code, const char *arg) {
    char b[128];
    size_t n = 0;
    const char *p = "err ";
    while (*p)
        b[n++] = *p++;
    p = code;
    while (*p && n < 120)
        b[n++] = *p++;
    if (arg && *arg && n < 120) {
        b[n++] = ' ';
        while (*arg && n < 126)
            b[n++] = *arg++;
    }
    b[n] = 0;
    pb_reply(c, b);
}
static int pb_append(char *b, size_t cap, size_t *n, const char *s) {
    while (*s) {
        if (*n + 1 >= cap)
            return 0;
        b[(*n)++] = *s++;
    }
    b[*n] = 0;
    return 1;
}
static int pb_num(char *b, size_t cap, size_t *n, uint32_t v) {
    char t[11];
    unsigned k = 0;
    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 10);
    while (k) {
        char x[2] = {t[--k], 0};
        if (!pb_append(b, cap, n, x))
            return 0;
    }
    return 1;
}
static void pb_safe_pin(pb_core *c, unsigned i) {
    if (c->pins[i].mode != PB_OFF) {
        if (c->h.set_mode)
            c->h.set_mode(c->h.ctx, pb_defs[i].port, pb_defs[i].bit, PB_HIZ, 0);
        c->pins[i].mode = PB_HIZ;
        c->pins[i].level = 0;
    }
    c->pins[i].listen = 0;
}
static void pb_safe_state(pb_core *c) {
    unsigned i;
    c->step_active = 0;
    c->step_done_pending = 0;
    for (i = 0; i < PB_NPINS; i++)
        pb_safe_pin(c, i);
}
static void pb_init(pb_core *c, const pb_hal *h) {
    memset(c, 0, sizeof(*c));
    c->h = *h;
    c->wdg = 5000;
    c->last_feed = pb_now(c);
}
/* Low-level printer primitives. All timing executes on the MCU, not over USB/WebSocket. */
static int pb_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!*s)
        return 0;
    while (*s) {
        if (*s < '0' || *s > '9')
            return 0;
        unsigned digit = (unsigned)(*s++ - '0');
        if (v > (UINT32_MAX - digit) / 10u)
            return 0;
        v = v * 10u + digit;
    }
    *out = v;
    return 1;
}
/* Read a physical input. Callers require a connected read_port HAL function. */
static unsigned pb_sample_pin(pb_core *c, unsigned index) {
    return (c->h.read_port(c->h.ctx, pb_defs[index].port) >> pb_defs[index].bit) & 1u;
}

/* The printer and GPIO command modules share this state and reply interface. */
#include "pb_printer.h"
#include "pb_commands.h"

static void pb_command(pb_core *c, char *line) {
    char **v = c->scratch_v;
    int n = pb_token(line, v, 16);
    uint32_t now = pb_now(c);
    c->reply[0] = 0;
    if (!n)
        return;
    if (n > 16) {
        pb_error(c, "arg", "");
        return;
    }
    if (c->dbg) {
        char message[48] = "";
        size_t len = 0;
        pb_append(message, sizeof message, &len, "#rx ");
        pb_append(message, sizeof message, &len, v[0]);
        pb_append(message, sizeof message, &len, " t=");
        pb_num(message, sizeof message, &len, now);
        pb_reply(c, message);
    }
    if (pb_eq(v[0], "PHEAD") || pb_eq(v[0], "PLATCH") || pb_eq(v[0], "PHEAT") ||
        pb_eq(v[0], "PSTEP") || pb_eq(v[0], "PSTEPSTAT"))
        pb_printer_command(c, v, n);
    else if (pb_eq(v[0], "HEL") || pb_eq(v[0], "DBG") || pb_eq(v[0], "WDG") || pb_eq(v[0], "RST"))
        pb_control_command(c, v, n, now);
    else if (pb_eq(v[0], "MOD"))
        pb_cmd_mode(c, v, n, now);
    else if (pb_eq(v[0], "SET"))
        pb_cmd_set(c, v, n, now);
    else if (pb_eq(v[0], "GET") || pb_eq(v[0], "STA"))
        pb_cmd_read(c, v, n, now);
    else if (pb_eq(v[0], "ADC"))
        pb_cmd_adc(c, v, n, now);
    else if (pb_eq(v[0], "LSN"))
        pb_cmd_listen(c, v, n, now);
    else
        pb_error(c, "cmd", v[0]);
}

static void pb_rx(pb_core *c, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        char x = (char)data[i];
        if (x == '\r')
            continue;
        if (x == '\n') {
            if (c->discard) {
                pb_reply(c, "err len");
                c->discard = 0;
                c->llen = 0;
                continue;
            }
            c->line[c->llen] = 0;
            char *s = c->line;
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s && *s != '#')
                pb_command(c, s);
            c->llen = 0;
            continue;
        }
        if (c->discard)
            continue;
        if (c->llen >= 128) {
            c->discard = 1;
            continue;
        }
        c->line[c->llen++] = x;
    }
}

static void pb_poll(pb_core *c) {
    uint32_t now = pb_now(c);
    if (c->step_done_pending) {
        c->step_done_pending = 0;
        pb_reply(c, "!step done");
    }
    unsigned i, port = c->pollport++ % 4;
    int anyout = 0;
    for (i = 0; i < PB_NPINS; i++) {
        if (c->pins[i].mode == PB_LO || c->pins[i].mode == PB_HI || c->pins[i].mode == PB_PWM)
            anyout = 1;
        if (c->pins[i].listen && pb_defs[i].port == port && c->h.read_port) {
            unsigned level = pb_sample_pin(c, i);
            if (level != c->pins[i].last) {
                char b[48];
                size_t n = 0;
                pb_append(b, sizeof b, &n, "!");
                pb_append(b, sizeof b, &n, pb_defs[i].name);
                pb_append(b, sizeof b, &n, "=");
                pb_append(b, sizeof b, &n, level ? "1" : "0");
                pb_append(b, sizeof b, &n, " @");
                pb_num(b, sizeof b, &n, now);
                pb_reply(c, b);
                c->pins[i].last = (uint8_t)level;
            }
        }
    }
    if (c->wdg && anyout && (uint32_t)(now - c->last_feed) >= c->wdg) {
        pb_safe_state(c);
        if (c->dbg)
            pb_reply(c, "#wdg trip");
        pb_reply(c, "!wdg");
        c->last_feed = now;
    }
}
#endif
