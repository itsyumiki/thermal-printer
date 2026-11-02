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
    /* Optional fast writer for ports not touched by interrupt-context code. */
    void (*write_port_fast)(void *, unsigned, uint16_t, uint16_t);
} pb_hal;
typedef struct {
    pb_hal h;
    pb_pin_state pins[PB_NPINS];
    char line[523]; /* TBP v1 maximum: 520 hex chars plus NUL */
    unsigned llen, discard, dbg, wdg;
    uint8_t tbp_active, tbp_next_id, tbp_last_id, tbp_last_state, tbp_last_valid;
    /* Zero means no accepted command is recorded for this id in this session. */
    uint8_t tbp_history[256];
    uint32_t last_feed;
    unsigned pollport;
    char reply[128];
    size_t rlen;
    volatile uint8_t step_active, step_reverse, step_stage, step_done_pending;
    volatile uint8_t
        step_phase; /* persistent half-step phase 0..7; survives PSTEP commands, reset by pb_init */
    volatile uint32_t step_remaining, step_wait_ms;
    uint32_t step_gap_ms, step_dwell_ms;
    uint32_t boot_ms, button_down_ms, led_change_ms;
    uint8_t power_ready, button_was_down, led_mode, led_phase, led_pulses;
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
    for (i = 0; i < PB_NPINS; i++) {
        /* The power latch, active-low button pull-up and status LED are local controls. */
        if (pb_eq(pb_defs[i].name, "PA1") || pb_eq(pb_defs[i].name, "PB1") || pb_eq(pb_defs[i].name, "PC7"))
            continue;
        pb_safe_pin(c, i);
    }
}
static void pb_init(pb_core *c, const pb_hal *h) {
    static const char *const names[] = {"PB10","PB9","PB8","PB11","PA9","PA10","PC3","PC2"};
    static const uint8_t levels[] = {0,0,1,0,0,1,1,0};
    int button, power, led;
    unsigned j;
    memset(c, 0, sizeof(*c));
    c->h = *h;
    c->wdg = 5000;
    c->last_feed = pb_now(c);
    c->boot_ms = c->last_feed;
    button = pb_pin("PB1"); power = pb_pin("PA1"); led = pb_pin("PC7");
    c->led_mode = 0;
    if (button >= 0 && c->h.set_mode) {
        c->h.set_mode(c->h.ctx,pb_defs[button].port,pb_defs[button].bit,PB_UP,1);
        c->pins[button].mode=PB_UP; c->pins[button].level=1;
    }
    if (power >= 0 && c->h.set_mode) {
        c->h.set_mode(c->h.ctx,pb_defs[power].port,pb_defs[power].bit,PB_HI,1);
        c->pins[power].mode=PB_HI; c->pins[power].level=1;
    }
    if (led >= 0 && c->h.set_mode) {
        c->h.set_mode(c->h.ctx,pb_defs[led].port,pb_defs[led].bit,PB_LO,0);
        c->pins[led].mode=PB_LO; c->pins[led].level=0;
    }
    for (j=0;j<sizeof names/sizeof names[0];j++) {
        int k=pb_pin(names[j]);
        if (k>=0 && c->h.set_mode) {
            unsigned mode=levels[j]?PB_HI:PB_LO;
            c->h.set_mode(c->h.ctx,pb_defs[k].port,pb_defs[k].bit,mode,levels[j]);
            c->pins[k].mode=(uint8_t)mode; c->pins[k].level=levels[j];
        }
    }
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

static void pb_printer_outputs_init(pb_core *c) {
    static const char *const names[] = {"PB10", "PB9", "PB8", "PB11", "PA9", "PA10", "PC3", "PC2"};
    static const uint8_t levels[] = {0, 0, 1, 0, 0, 1, 1, 0};
    unsigned j;
    for (j = 0; j < sizeof names / sizeof names[0]; j++) {
        int k = pb_pin(names[j]);
        if (k >= 0 && c->h.set_mode) {
            unsigned mode = levels[j] ? PB_HI : PB_LO;
            c->h.set_mode(c->h.ctx, pb_defs[k].port, pb_defs[k].bit, mode, levels[j]);
            c->pins[k].mode = (uint8_t)mode;
            c->pins[k].level = levels[j];
        }
    }
}
static void pb_motor_outputs_init(pb_core *c) {
    unsigned i;
    for (i = 0; i < 4; i++) {
        int k = pb_pin(pb_motor_pins[i]);
        unsigned level = pb_motor_phases[c->step_phase & 7u][i];
        if (k >= 0 && c->h.set_mode) {
            unsigned mode = level ? PB_HI : PB_LO;
            c->h.set_mode(c->h.ctx, pb_defs[k].port, pb_defs[k].bit, mode, level);
            c->pins[k].mode = (uint8_t)mode;
            c->pins[k].level = (uint8_t)level;
        }
    }
}

/* PB1 is active-low. Hold one second at boot to latch PA1; a fresh one-second
   hold after startup cuts PA1. PC7 slow-blinks until HELLO, flashes twice, then stays on. */
static void pb_power_led_poll(pb_core *c, uint32_t now) {
    int bi = pb_pin("PB1"), pi = pb_pin("PA1"), li = pb_pin("PC7");
    unsigned pressed;
    if (bi < 0 || pi < 0 || li < 0 || !c->h.read_port)
        return;
    pressed = !pb_sample_pin(c, (unsigned)bi);
    if (c->button_was_down == 2u) {
        /* A completed hold must be released before another hold can begin. */
        if (!pressed)
            c->button_was_down = 0;
    } else if (!c->power_ready) {
        if (pressed) {
            if (!c->button_was_down) {
                c->button_was_down = 1;
                c->button_down_ms = now;
            } else if ((uint32_t)(now - c->button_down_ms) >= 1000u) {
                c->power_ready = 1;
                c->button_was_down = 2u;
                (void)pb_printer_pin(c, "PA1", 1);
            }
        } else {
            c->button_was_down = 0;
            if ((uint32_t)(now - c->boot_ms) >= 1000u)
                (void)pb_printer_pin(c, "PA1", 0);
        }
    } else if (pressed) {
        if (!c->button_was_down) {
            c->button_was_down = 1;
            c->button_down_ms = now;
        } else if ((uint32_t)(now - c->button_down_ms) >= 1000u) {
            (void)pb_printer_pin(c, "PB11", 0);
            c->step_active = 0;
            (void)pb_printer_pin(c, "PA1", 0);
            c->power_ready = 0;
            c->button_was_down = 2u;
            c->led_mode = 0;
            c->led_phase = 0;
            c->led_change_ms = now;
        }
    } else
        c->button_was_down = 0;
    if (c->led_mode == 1) {
        if ((uint32_t)(now - c->led_change_ms) >= 100u) {
            c->led_change_ms = now;
            c->led_phase++;
            (void)pb_printer_pin(c, "PC7", (c->led_phase == 1 || c->led_phase == 3) ? 1 : 0);
            if (c->led_phase >= 4)
                c->led_mode = 3;
        }
    } else if (c->led_mode == 0 && (uint32_t)(now - c->led_change_ms) >= 500u) {
        c->led_change_ms = now;
        c->led_phase ^= 1u;
        (void)pb_printer_pin(c, "PC7", c->led_phase);
    } else if (c->led_mode == 3 && (uint32_t)(now - c->led_change_ms) >= 100u) {
        c->led_mode = 2;
        (void)pb_printer_pin(c, "PC7", 1);
    } else if (c->led_mode == 2)
        (void)pb_printer_pin(c, "PC7", 1);
}

static uint16_t pb_tbp_crc(const uint8_t *p, size_t n) {
    uint16_t crc = 0xffffu;
    size_t i;
    unsigned bit;
    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}
static void pb_tbp_send(pb_core *c, uint8_t id, uint8_t kind, const uint8_t *data, uint8_t len) {
    static const char hx[] = "0123456789ABCDEF";
    uint8_t raw[260];
    char out[523];
    size_t i, n = (size_t)len + 5u;
    uint16_t crc;
    raw[0] = id;
    raw[1] = kind;
    raw[2] = len;
    if (len && data)
        memcpy(raw + 3, data, len);
    crc = pb_tbp_crc(raw, n - 2u);
    raw[n - 2u] = (uint8_t)(crc >> 8);
    raw[n - 1u] = (uint8_t)crc;
    for (i = 0; i < n; i++) {
        out[i * 2] = hx[raw[i] >> 4];
        out[i * 2 + 1] = hx[raw[i] & 15u];
    }
    out[n * 2] = '\n';
    out[n * 2 + 1] = 0;
    pb_emit(c, out);
}
static void pb_tbp_err(pb_core *c, uint8_t id, uint8_t code, uint8_t arg) {
    uint8_t d[2] = {code, arg};
    pb_tbp_send(c, id, 0x83u, d, 2);
}
static int pb_tbp_hexval(char x) {
    if (x >= '0' && x <= '9')
        return x - '0';
    if (x >= 'a' && x <= 'f')
        return x - 'a' + 10;
    if (x >= 'A' && x <= 'F')
        return x - 'A' + 10;
    return -1;
}
static void pb_tbp_status(pb_core *c, uint8_t id, uint8_t state, const uint8_t *body, uint8_t n) {
    uint8_t d[64];
    d[0] = state;
    if (n)
        memcpy(d + 1, body, n);
    pb_tbp_send(c, id, 0x82u, d, (uint8_t)(n + 1u));
}
static void pb_tbp_accept(pb_core *c, uint8_t id) {
    unsigned evict = (((unsigned)id - 1u + 255u - 127u) % 255u) + 1u;
    c->tbp_last_id = id;
    c->tbp_last_state = 3;
    c->tbp_last_valid = 1;
    c->tbp_history[evict] = 0;
    c->tbp_history[id] = c->tbp_last_state;
    c->tbp_next_id = (uint8_t)(id + 1u);
    if (!c->tbp_next_id)
        c->tbp_next_id = 1;
}
static int pb_tbp_line(pb_core *c, const char *line) {
    uint8_t raw[260], id, op, len, body[48];
    size_t chars = strlen(line), n, i;
    uint16_t crc;
    int allhex = 1;
    for (i = 0; i < chars; i++) {
        if (pb_tbp_hexval(line[i]) < 0) {
            allhex = 0;
            break;
        }
    }
    if (!allhex) {
        /* Legacy commands are word-led. A line with four leading hex digits
         * is treated as a TBP candidate, so mixed-hex corruption cannot fall
         * through into the legacy command parser with a misleading error. */
        if (chars >= 4 && pb_tbp_hexval(line[0]) >= 0 &&
            pb_tbp_hexval(line[1]) >= 0 && pb_tbp_hexval(line[2]) >= 0 &&
            pb_tbp_hexval(line[3]) >= 0) {
            uint8_t id = (uint8_t)((pb_tbp_hexval(line[0]) << 4) | pb_tbp_hexval(line[1]));
            pb_tbp_err(c, id, 2, 0);
            return 1;
        }
        return 0;
    }
    if ((chars & 1u) || chars < 10u || chars > 520u) {
        pb_tbp_err(c, 0, 2, 0);
        return 1;
    }
    n = chars / 2u;
    for (i = 0; i < n; i++)
        raw[i] = (uint8_t)((pb_tbp_hexval(line[i * 2]) << 4) | pb_tbp_hexval(line[i * 2 + 1]));
    id = raw[0];
    op = raw[1];
    len = raw[2];
    if (n != (size_t)len + 5u) {
        pb_tbp_err(c, id, 4, len);
        return 1;
    }
    crc = pb_tbp_crc(raw, n - 2u);
    if (crc != (uint16_t)(((uint16_t)raw[n - 2u] << 8) | raw[n - 1u])) {
        pb_tbp_err(c, id, 1, 0);
        return 1;
    }
    c->last_feed = pb_now(c);
    if (op == 0x01u) {
        uint8_t info[38] = {1,    0,    0,    0,    0,    1,    0x80, 1,    1,    0x03,
                            0xE8, 0x03, 0xE8, 0,    1,    0x17, 0x70, 0,    100,  0x03,
                            0xE8, 0x75, 0x30, 0x0B, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0,
                            0,    0,    0,    0,    0,    0,    0,    0};
        if (len != 1 || raw[3] != 1) {
            pb_tbp_err(c, id, 4, 1);
            return 1;
        }
        c->tbp_active = 1;
        c->tbp_next_id = (uint8_t)(id + 1u);
        if (!c->tbp_next_id)
            c->tbp_next_id = 1;
        c->tbp_last_valid = 0;
        memset(c->tbp_history, 0, sizeof c->tbp_history);
        c->step_active = 0;
        pb_printer_outputs_init(c);
        c->led_mode = 1;
        c->led_phase = 0;
        c->led_change_ms = pb_now(c);
        (void)pb_printer_pin(c, "PB11", 0);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        pb_tbp_status(c, id, 3, info, sizeof info);
        return 1;
    }
    if (op == 0xFFu) {
        if (!id || len) {
            pb_tbp_err(c, id, 4, 0);
            return 1;
        }
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        (void)pb_printer_pin(c, "PB11", 0);
        c->step_active = 0;
        for (i = 0; i < 4; i++) {
            int k = pb_pin(pb_motor_pins[i]);
            if (k >= 0)
                pb_safe_pin(c, (unsigned)k);
        }
        body[0] = 3;
        pb_tbp_status(c, id, 3, body, 1);
        return 1;
    }
    if (!c->tbp_active) {
        pb_tbp_err(c, id, 10, 0);
        return 1;
    }
    if (id != c->tbp_next_id) {
        /* IDs form the 1..255 cycle. Distances 128..254 are old IDs;
         * only replay one when it was accepted in this HELLO session. */
        unsigned delta = ((unsigned)id + 255u - c->tbp_next_id) % 255u;
        if (delta >= 128u && c->tbp_history[id]) {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
            pb_tbp_status(c, id, c->tbp_history[id], 0, 0);
            return 1;
        }
        pb_tbp_err(c, id, 6, c->tbp_next_id);
        return 1;
    }
    if (op == 0x02u) {
        uint8_t stat[13] = {0, 0, 1, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
        uint8_t lastid = c->tbp_last_valid ? c->tbp_last_id : 0;
        if (len) {
            pb_tbp_err(c, id, 4, 0);
            return 1;
        }
        pb_tbp_accept(c, id);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        stat[4] = lastid;
        if (pb_all_outputs(c, pb_motor_pins, 4))
            stat[12] |= 4u;
        pb_tbp_status(c, id, 3, stat, sizeof stat);
        return 1;
    }
    if (op == 0x03u) {
        if (len) {
            pb_tbp_err(c, id, 4, 0);
            return 1;
        }
        pb_tbp_accept(c, id);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        (void)pb_printer_pin(c, "PB11", 0);
        c->step_active = 0;
        body[0] = 3;
        body[1] = 0;
        pb_tbp_status(c, id, 3, body, 2);
        return 1;
    }
    if (op == 0x04u) {
        uint16_t value;
        if (len != 3) {
            pb_tbp_err(c, id, 4, 3);
            return 1;
        }
        value = (uint16_t)(((uint16_t)raw[4] << 8) | raw[5]);
        if ((raw[3] == 1u && value != 0u && (value < 100u || value > 60000u)) ||
            (raw[3] == 3u && value != 0u && (value < 500u || value > 60000u)) || raw[3] < 1u ||
            raw[3] > 3u) {
            pb_tbp_err(c, id, 5, 0);
            return 1;
        }
        pb_tbp_accept(c, id);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        body[0] = 3;
        body[1] = raw[3];
        body[2] = raw[4];
        body[3] = raw[5];
        pb_tbp_status(c, id, 3, body, 4);
        return 1;
    }
    if (op == 0x20u) {
        if (len != 1 || raw[3] > 1) {
            pb_tbp_err(c, id, 4, 1);
            return 1;
        }
        if (c->step_active) {
            pb_tbp_err(c, id, 9, 0);
            return 1;
        }
        pb_tbp_accept(c, id);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        if (raw[3]) {
            pb_printer_outputs_init(c);
            pb_drive_motor_phase(c, c->step_phase & 7u);
        } else {
            for (i = 0; i < 4; i++) {
                int k = pb_pin(pb_motor_pins[i]);
                if (k >= 0)
                    pb_safe_pin(c, (unsigned)k);
            }
        }
        body[0] = 3;
        pb_tbp_status(c, id, 3, body, 1);
        return 1;
    }
    if (op == 0x10u) {
        static const char *const hp[] = {"PB10", "PB9", "PB8", "PB11"};
        static const char *const mp[] = {"PA9", "PA10", "PC3", "PC2"};
        char hex[97];
        uint16_t burn, period;
        int steps;
        unsigned j, slices;
        int any = 0;
        uint8_t result[12] = {3, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0};
        if (len != 54) {
            pb_tbp_err(c, id, 4, 54);
            return 1;
        }
        burn = (uint16_t)(((uint16_t)raw[51] << 8) | raw[52]);
        steps = (int8_t)raw[53];
        period = (uint16_t)(((uint16_t)raw[54] << 8) | raw[55]);
        slices = raw[56];
        if (burn > 6000u || slices < 1u ||
            slices > (steps ? (unsigned)(steps < 0 ? -steps : steps) : 1u) ||
            (steps && period < 1000u)) {
            pb_tbp_err(c, id, 5, steps ? 51 : 48);
            return 1;
        }
        if (steps && !pb_all_outputs(c, mp, 4))
            pb_motor_outputs_init(c);
        if (steps && !pb_all_outputs(c, mp, 4)) {
            pb_tbp_err(c, id, 9, 0);
            return 1;
        }
        for (j = 0; j < 48; j++) {
            if (raw[3 + j])
                any = 1;
            hex[j * 2] = "0123456789ABCDEF"[raw[3 + j] >> 4];
            hex[j * 2 + 1] = "0123456789ABCDEF"[raw[3 + j] & 15u];
        }
        hex[96] = 0;
        if (any && burn && !pb_all_outputs(c, hp, 4)) {
            pb_tbp_err(c, id, 9, 0);
            return 1;
        }
        pb_tbp_accept(c, id);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        if (any && burn) {
            pb_shift_head_row(c, hex, 1, 0);
            (void)pb_printer_pin(c, "PB8", 0);
            pb_wait_us(c, 10);
            (void)pb_printer_pin(c, "PB8", 1);
            for (j = 0; j < slices; j++) {
                unsigned on = (unsigned)burn / slices + (j < ((unsigned)burn % slices));
                if (on) {
                    (void)pb_printer_pin(c, "PB11", 1);
                    pb_wait_us(c, on);
                    (void)pb_printer_pin(c, "PB11", 0);
                }
                if (j + 1u < slices)
                    pb_wait_us(c, 100u);
            }
        }
        if (steps) {
            pb_step_start(c, steps < 0, (unsigned)(steps < 0 ? -steps : steps) + 1u, 0, period);
            while (c->step_active)
                pb_wait_us(c, 1000u);
        }
        result[7] = (uint8_t)(burn >> 8);
        result[8] = (uint8_t)burn;
        {
            uint32_t pe = steps ? (((uint32_t)period + 999u) / 1000u) * 1000u : 0u;
            if (pe > 65535u)
                pe = 65535u;
            result[9] = (uint8_t)(pe >> 8);
            result[10] = (uint8_t)pe;
        }
        pb_tbp_status(c, id, 3, result, sizeof result);
        return 1;
    }
    if (op == 0x11u) {
        int32_t steps;
        uint16_t period;
        uint8_t result[3] = {3, 0, 0};
        if (len != 6) {
            pb_tbp_err(c, id, 4, 6);
            return 1;
        }
        steps = (int32_t)(((uint32_t)raw[3] << 24) | ((uint32_t)raw[4] << 16) |
                          ((uint32_t)raw[5] << 8) | raw[6]);
        period = (uint16_t)(((uint16_t)raw[7] << 8) | raw[8]);
        if (!steps || steps > 10000 || steps < -10000 || period < 1000u) {
            pb_tbp_err(c, id, 5, 0);
            return 1;
        }
        if (!pb_all_outputs(c, pb_motor_pins, 4))
            pb_motor_outputs_init(c);
        if (!pb_all_outputs(c, pb_motor_pins, 4)) {
            pb_tbp_err(c, id, 9, 0);
            return 1;
        }
        pb_tbp_accept(c, id);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        pb_step_start(c, steps < 0, (unsigned)(steps < 0 ? -steps : steps) + 1u, 0, period);
        while (c->step_active)
            pb_wait_us(c, 1000u);
        {
            uint32_t pe = (((uint32_t)period + 999u) / 1000u) * 1000u;
            if (pe > 65535u)
                pe = 65535u;
            result[1] = (uint8_t)(pe >> 8);
            result[2] = (uint8_t)pe;
        }
        pb_tbp_status(c, id, 3, result, sizeof result);
        return 1;
    }
    if (op == 0x12u) {
        uint16_t ms;
        if (len != 2) {
            pb_tbp_err(c, id, 4, 2);
            return 1;
        }
        ms = (uint16_t)(((uint16_t)raw[3] << 8) | raw[4]);
        if (!ms) {
            pb_tbp_err(c, id, 5, 0);
            return 1;
        }
        pb_tbp_accept(c, id);
        {
            uint8_t ack = 0;
            pb_tbp_send(c, id, 0x81u, &ack, 1);
        }
        while (ms--)
            pb_wait_us(c, 1000u);
        body[0] = 3;
        pb_tbp_status(c, id, 3, body, 1);
        return 1;
    }
    pb_tbp_err(c, id, 3, op);
    return 1;
}

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
        pb_eq(v[0], "PSTEP") || pb_eq(v[0], "PSTEPSTAT") || pb_eq(v[0], "PEVENT"))
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
            if (*s && *s != '#' && !pb_tbp_line(c, s))
                pb_command(c, s);
            c->llen = 0;
            continue;
        }
        if (c->discard)
            continue;
        if (c->llen >= 128) {
            size_t j;
            int hex_prefix = 1;
            for (j=0;j<c->llen;j++) if (pb_tbp_hexval(c->line[j])<0) { hex_prefix=0; break; }
            if (!hex_prefix || pb_tbp_hexval(x)<0 || c->llen>=520) {
                c->discard = 1;
                continue;
            }
        }
        c->line[c->llen++] = x;
    }
}

static void pb_poll(pb_core *c) {
    uint32_t now = pb_now(c);
    pb_power_led_poll(c, now);
    if (c->step_done_pending) {
        c->step_done_pending = 0;
        pb_reply(c, "!step done");
    }
    unsigned i, port = c->pollport++ % 4;
    int anyout = 0;
    for (i = 0; i < PB_NPINS; i++) {
        if ((c->pins[i].mode == PB_LO || c->pins[i].mode == PB_HI || c->pins[i].mode == PB_PWM) &&
            !pb_eq(pb_defs[i].name, "PA1") && !pb_eq(pb_defs[i].name, "PC7")) anyout = 1;
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
        c->tbp_active=0; c->led_mode=0; c->led_phase=0; c->led_change_ms=now;
        if (c->dbg)
            pb_reply(c, "#wdg trip");
        pb_reply(c, "!wdg");
        c->last_feed = now;
    }
}
#endif
