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
typedef enum {
  PB_OFF,
  PB_HIZ,
  PB_IN,
  PB_UP,
  PB_DN,
  PB_LO,
  PB_HI,
  PB_ADC,
  PB_PWM
} pb_mode;
typedef struct {
  const char *name;
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
  volatile uint8_t step_phase; /* persistent half-step phase 0..7; survives PSTEP commands, reset by pb_init */
  volatile uint32_t step_remaining, step_wait_ms;
  uint32_t step_gap_ms, step_dwell_ms;
  int scratch_p[PB_MAX_PINS];
  char *scratch_v[17];
} pb_core;
static const char *pb_modes[] = {"off", "hiz", "in",  "up", "dn",
                                 "lo",  "hi",  "adc", "pwm"};
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
  unsigned i;
  memset(c, 0, sizeof(*c));
  c->h = *h;
  c->wdg = 5000;
  c->last_feed = pb_now(c);
  for (i = 0; i < PB_NPINS; i++) {
    c->pins[i].name = pb_defs[i].name;
    c->pins[i].mode = PB_OFF;
  }
}
/* Low-level printer primitives. All timing executes on the MCU, not over USB/WebSocket. */
static int pb_u32(const char *s, uint32_t *out) {
  uint32_t v = 0;
  if (!*s) return 0;
  while (*s) {
    if (*s < '0' || *s > '9') return 0;
    { unsigned d = (unsigned)(*s++ - '0'); if (v > 429496729u || (v == 429496729u && d > 5u)) return 0; v = v * 10u + d; }
  }
  *out = v;
  return 1;
}
static int pb_hexbyte(const char *s, unsigned *out) {
  unsigned v = 0, i;
  for (i = 0; i < 2; i++) {
    char x = s[i]; unsigned d;
    if (x >= '0' && x <= '9') d = (unsigned)(x - '0');
    else if (x >= 'a' && x <= 'f') d = (unsigned)(x - 'a' + 10);
    else if (x >= 'A' && x <= 'F') d = (unsigned)(x - 'A' + 10);
    else return 0;
    v = (v << 4) | d;
  }
  *out = v; return 1;
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
  if (us && c->h.delay_us) c->h.delay_us(c->h.ctx, us);
}
static int pb_all_outputs(pb_core *c, const char *const *pins, unsigned n) {
  unsigned i;
  for (i = 0; i < n; i++) {
    int k = pb_pin(pins[i]);
    if (k < 0 || (c->pins[k].mode != PB_LO && c->pins[k].mode != PB_HI)) return 0;
  }
  return 1;
}
static void pb_printer_command(pb_core *c, char **v, int n) {
  static const char *const headpins[] = {"PB10", "PB9", "PB8", "PB11"};
  static const char *const motorpins[] = {"PA9", "PA10", "PC3", "PC2"};
  /* Official JieLi AC6956C half-step sequence on PA9, PA10, PC3, PC2.
   Static firmware trace only; pin polarity and motor wiring remain unverified. */
  static const uint8_t phases[8][4] = {
    {0,1,1,0}, {0,1,0,0}, {0,1,0,1}, {0,0,0,1},
    {1,0,0,1}, {1,0,0,0}, {1,0,1,0}, {0,0,1,0}
  };
  uint32_t a, b, count, i, j; char *hex;
  if (!c->h.delay_us) { pb_error(c, "bsy", "delay"); return; }
  if (pb_eq(v[0], "PHEAD")) {
    if (n != 4 || strlen(v[1]) != 96 || !pb_u32(v[2], &a) || !pb_u32(v[3], &b) || !pb_all_outputs(c, headpins, 4)) { pb_error(c, "arg", "PHEAD"); return; }
    hex = v[1];
    for (i = 0; i < 48; i++) {
      unsigned byte;
      if (!pb_hexbyte(hex + i*2, &byte)) { pb_error(c, "hex", "PHEAD"); return; }
    }
    /* Fast path: resolve DATA/CLK once and write the ports directly. Name lookups
       plus per-write bookkeeping made a row take ~76 ms of pure overhead. */
    int pin_data = pb_pin("PB10"), pin_clk = pb_pin("PB9");
    unsigned dport = pb_defs[pin_data].port, cport = pb_defs[pin_clk].port;
    uint16_t dmask = (uint16_t)(1u << pb_defs[pin_data].bit);
    uint16_t cmask = (uint16_t)(1u << pb_defs[pin_clk].bit);
    unsigned last = 0;
    if (!c->h.write_port) { pb_error(c, "mod", "PB10"); return; }
    for (i = 0; i < 48; i++) {
      unsigned byte; (void)pb_hexbyte(hex + i*2, &byte);
      for (j = 0; j < 8; j++) {
        last = (byte >> j) & 1u;
        c->h.write_port(c->h.ctx, dport, dmask, last ? dmask : 0);
        pb_wait_us(c, (unsigned)b); /* data setup / hold */
        c->h.write_port(c->h.ctx, cport, cmask, cmask); pb_wait_us(c, (unsigned)a);
        c->h.write_port(c->h.ctx, cport, cmask, 0); pb_wait_us(c, (unsigned)a);
      }
    }
    c->pins[pin_data].level = (uint8_t)last; c->pins[pin_data].mode = (uint8_t)(last ? PB_HI : PB_LO);
    c->pins[pin_clk].level = 0; c->pins[pin_clk].mode = PB_LO;
    pb_reply(c, "ok bits=384"); c->last_feed = pb_now(c); return;
  }
  if (pb_eq(v[0], "PLATCH")) {
    if (n != 2 || !pb_u32(v[1], &a) || !pb_all_outputs(c, headpins, 4)) { pb_error(c, "arg", "PLATCH"); return; }
    (void)pb_printer_pin(c, "PB8", 0); pb_wait_us(c, (unsigned)a);
    (void)pb_printer_pin(c, "PB8", 1); pb_reply(c, "ok"); c->last_feed = pb_now(c); return;
  }
  if (pb_eq(v[0], "PHEAT")) {
    /* Experimental single pulse. Duration policy belongs to the host-side demo. */
    if (n != 2 || !pb_u32(v[1], &a) || !pb_all_outputs(c, headpins, 4)) { pb_error(c, "arg", "PHEAT"); return; }
    (void)pb_printer_pin(c, "PB11", 1); pb_wait_us(c, (unsigned)a);
    (void)pb_printer_pin(c, "PB11", 0); pb_reply(c, "ok"); c->last_feed = pb_now(c); return;
  }
  if (pb_eq(v[0], "PSTEP")) {
    unsigned phase, k;
    if (n != 5 || (!pb_eq(v[1], "FWD") && !pb_eq(v[1], "REV")) || !pb_u32(v[2], &count) || !pb_u32(v[3], &a) || !pb_u32(v[4], &b) || !pb_all_outputs(c, motorpins, 4) || c->step_active) { pb_error(c, "arg", "PSTEP"); return; }
    /* Nonblocking 1 ms scheduler: USB command handling and watchdog polling
       continue during motion. Host-side code owns duration/range policy. */
    c->step_reverse = (uint8_t)pb_eq(v[1], "REV");
    c->step_remaining = count;
    c->step_gap_ms = (uint32_t)(((uint64_t)a + 999u) / 1000u);
    c->step_dwell_ms = (uint32_t)(((uint64_t)b + 999u) / 1000u);
    c->step_stage = 0;
    c->step_wait_ms = c->step_dwell_ms;
    if (c->step_wait_ms == 0) c->step_wait_ms = 1;
    c->step_active = count != 0;
    c->last_feed = pb_now(c);
    if (count) {
      phase = c->step_phase & 7u; /* resume from the remembered phase */
      for (k = 0; k < 4; k++) (void)pb_printer_pin(c, motorpins[k], phases[phase][k]);
      pb_reply(c, "ok started");
    } else pb_reply(c, "ok halfsteps");
    return;
  }
  if (pb_eq(v[0], "PSTEPSTAT")) {
    if (n != 1) { pb_error(c, "arg", "PSTEPSTAT"); return; }
    pb_reply(c, c->step_active ? "ok busy" : "ok done");
    c->last_feed = pb_now(c);
    return;
  }
}

static void pb_command(pb_core *c, char *line) {
  char **v = c->scratch_v;
  int n = pb_token(line, v, 16), *p = c->scratch_p, i, j;
  uint32_t now = pb_now(c);
  char *b = c->reply;
  size_t z = 0;
  b[0] = 0;
  if (!n)
    return;
  if (n > 16) {
    pb_error(c, "arg", "");
    return;
  }
  if (c->dbg) {
    char d[48];
    size_t q = 0;
    pb_append(d, sizeof d, &q, "#rx ");
    pb_append(d, sizeof d, &q, v[0]);
    pb_append(d, sizeof d, &q, " t=");
    pb_num(d, sizeof d, &q, now);
    pb_reply(c, d);
  }
  if (pb_eq(v[0], "PHEAD") || pb_eq(v[0], "PLATCH") || pb_eq(v[0], "PHEAT") || pb_eq(v[0], "PSTEP") || pb_eq(v[0], "PSTEPSTAT")) {
    pb_printer_command(c, v, n);
    return;
  }
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
    char *e;
    unsigned long x = 0;
    if (n != 2) {
      pb_error(c, "arg", "");
      return;
    }
    for (i = 0; v[1][i]; i++) {
      if (v[1][i] < '0' || v[1][i] > '9' || x > 3600000) {
        pb_error(c, "arg", v[1]);
        return;
      }
      x = x * 10 + (unsigned)(v[1][i] - '0');
    }
    if (x != 0 && (x < 100 || x > 3600000)) {
      pb_error(c, "arg", v[1]);
      return;
    }
    (void)e;
    c->wdg = (unsigned)x;
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
  if (pb_eq(v[0], "MOD")) {
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
    if (m == PB_PWM && !c->h.pwm_set) {
      pb_error(c, "bsy", v[1]);
      return;
    }
    if (m == PB_ADC && c->h.adc_read &&
        c->h.adc_read(c->h.ctx, pb_defs[i].adc, 0, 0) < 0) {
      pb_error(c, "bsy", v[1]);
      return;
    }
    if (c->h.set_mode &&
        c->h.set_mode(c->h.ctx, pb_defs[i].port, pb_defs[i].bit, (unsigned)m,
                      m == PB_HI) < 0) {
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
  if (pb_eq(v[0], "SET")) {
    if (n < 2) {
      pb_error(c, "arg", "");
      return;
    }
    unsigned masks[4] = {0}, vals[4] = {0};
    uint16_t setbits = 0;
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
      for (j = 1; j < i; j++)
        if (p[j - 1] == p[i - 1]) {
          pb_error(c, "arg", v[i]);
          return;
        }
      if (c->pins[p[i - 1]].mode != PB_LO && c->pins[p[i - 1]].mode != PB_HI) {
        pb_error(c, "mod", v[i]);
        return;
      }
      masks[pb_defs[p[i - 1]].port] |= (1u << pb_defs[p[i - 1]].bit);
      if (*eq == '1') {
        setbits |= (uint16_t)(1u << (i - 1));
        vals[pb_defs[p[i - 1]].port] |= (1u << pb_defs[p[i - 1]].bit);
      }
    }
    for (i = 0; i < 4; i++) {
      if (masks[i] && c->h.write_port)
        c->h.write_port(c->h.ctx, i, (uint16_t)masks[i], (uint16_t)vals[i]);
    }
    for (i = 1; i < n; i++) {
      int k = p[i - 1];
      c->pins[k].level = (uint8_t)((setbits >> (i - 1)) & 1u);
      c->pins[k].mode = c->pins[k].level ? PB_HI : PB_LO;
    }
    pb_reply(c, "ok");
    c->last_feed = now;
    return;
  }
  if (pb_eq(v[0], "GET") || pb_eq(v[0], "STA")) {
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
      for (i = 0; i < n; i++)
        p[i] = p[i];
    } /* Preflight reply length: never silently truncate a protocol line. */
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
        if (!sta && c->pins[k].mode == PB_ADC) {
          pb_error(c, "mod", pb_defs[k].name);
          return;
        }
        need += 1 + strlen(pb_defs[k].name) + 1 +
                (sta ? strlen(pb_modes[c->pins[k].mode]) : 1);
      }
      if (need > 127) {
        pb_error(c, "len", "");
        return;
      }
    }
    pb_append(b, sizeof(c->reply), &z, "ok");
    for (i = 0; i < n; i++) {
      int k = p[i];
      if (!sta && (c->pins[k].mode < PB_IN || c->pins[k].mode > PB_HI)) {
        pb_error(c, "mod", pb_defs[k].name);
        return;
      }
      if (sta && c->pins[k].mode == PB_OFF)
        continue;
      if (!sta && c->pins[k].mode == PB_ADC) {
        pb_error(c, "mod", pb_defs[k].name);
        return;
      }
      pb_append(b, sizeof(c->reply), &z, " ");
      pb_append(b, sizeof(c->reply), &z, pb_defs[k].name);
      pb_append(b, sizeof(c->reply), &z, "=");
      if (sta) {
        pb_append(b, sizeof(c->reply), &z, pb_modes[c->pins[k].mode]);
      } else {
        unsigned level = c->pins[k].level;
        /* Input reads must sample the physical pad, not the output shadow.
         * The shadow is only meaningful for output modes. */
        if (c->pins[k].mode == PB_IN || c->pins[k].mode == PB_UP ||
            c->pins[k].mode == PB_DN) {
          if (!c->h.read_port) {
            pb_error(c, "bsy", pb_defs[k].name);
            return;
          }
          level = (c->h.read_port(c->h.ctx, pb_defs[k].port) >>
                   pb_defs[k].bit) & 1u;
          c->pins[k].last = (uint8_t)level;
        }
        pb_append(b, sizeof(c->reply), &z, level ? "1" : "0");
      }
    }
    pb_reply(c, b);
    c->last_feed = now;
    return;
  }
  if (pb_eq(v[0], "ADC")) {
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
      pb_append(b, sizeof(c->reply), &z,
                p[i - 1] == -2 ? "vbat" : pb_defs[p[i - 1]].name);
      pb_append(b, sizeof(c->reply), &z, "=");
      pb_num(b, sizeof(c->reply), &z, raw);
      pb_append(b, sizeof(c->reply), &z, "/");
      pb_num(b, sizeof(c->reply), &z, mv);
    }
    pb_reply(c, b);
    c->last_feed = now;
    return;
  }
  if (pb_eq(v[0], "LSN")) {
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
          c->pins[k].last =
              (c->h.read_port(c->h.ctx, pb_defs[k].port) >> pb_defs[k].bit) & 1;
      }
    }
    pb_reply(c, "ok");
    c->last_feed = now;
    return;
  }
  if (pb_eq(v[0], "PWM")) {
    pb_error(c, "cmd", v[0]);
    return;
  }
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
static inline void pb_step_tick(pb_core *c) {
  static const char *const pins[4] = {"PA9", "PA10", "PC3", "PC2"};
  static const uint8_t seq[8][4] = {{0,1,1,0},{0,1,0,0},{0,1,0,1},{0,0,0,1},{1,0,0,1},{1,0,0,0},{1,0,1,0},{0,0,1,0}};
  unsigned k, phase;
  if (!c->step_active) return;
  c->last_feed = pb_now(c);
  if (c->step_wait_ms > 1) { c->step_wait_ms--; return; }
  c->step_wait_ms = 0;
  if (!c->step_stage) {
    c->step_stage = 1;
    if (c->step_gap_ms) { c->step_wait_ms = c->step_gap_ms; return; }
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
  for (k = 0; k < 4; k++) (void)pb_printer_pin(c, pins[k], seq[phase][k]);
  c->step_stage = 0;
  c->step_wait_ms = c->step_dwell_ms;
  if (!c->step_wait_ms) c->step_wait_ms = 1;
}

static void pb_poll(pb_core *c) {
  uint32_t now = pb_now(c);
  if (c->step_done_pending) { c->step_done_pending = 0; pb_reply(c, "!step done"); }
  unsigned i, port = c->pollport++ % 4;
  int anyout = 0;
  for (i = 0; i < PB_NPINS; i++) {
    if (c->pins[i].mode == PB_LO || c->pins[i].mode == PB_HI ||
        c->pins[i].mode == PB_PWM)
      anyout = 1;
    if (c->pins[i].listen && pb_defs[i].port == port && c->h.read_port) {
      unsigned level = (c->h.read_port(c->h.ctx, port) >> pb_defs[i].bit) & 1;
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
