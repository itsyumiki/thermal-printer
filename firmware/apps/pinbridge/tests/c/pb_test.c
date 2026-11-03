#ifndef PB_PINS_HEADER
#define PB_PINS_HEADER "pb_pins_test.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../../src/pb_core.h"
static unsigned now_ms, writes;
static unsigned delay_remainder_us;
static pb_core *active_core;
static uint16_t outs[4], pads[4];
static char output[32768];
static size_t used;
static int mode(void *x, unsigned p, unsigned b, unsigned m, unsigned l) {
    (void)x;
    (void)m;
    if (l)
        outs[p] |= (uint16_t)(1u << b);
    else
        outs[p] &= (uint16_t)~(1u << b);
    return 0;
}
static void wr(void *x, unsigned p, uint16_t mask, uint16_t val) {
    (void)x;
    outs[p] = (uint16_t)((outs[p] & ~mask) | (val & mask));
    writes++;
}
static uint16_t rd(void *x, unsigned p) {
    (void)x;
    return (uint16_t)(pads[p] | outs[p]);
}
/* Fake sensors: head raw 412 (~19 degC), battery 4000 mV; @adc directives change them. */
static unsigned fake_head_raw = 412, fake_vbat_mv = 4000, fake_adc_fail;
static int adc(void *x, int ch, unsigned *r, unsigned *m) {
    (void)x;
    if ((ch == -2 && (fake_adc_fail & 2u)) || (ch == 10 && (fake_adc_fail & 1u)))
        return -1;
    if (r)
        *r = ch == -2 ? fake_vbat_mv / 8u : ch == 10 ? fake_head_raw : 412;
    if (m)
        *m = ch == -2 ? fake_vbat_mv : ch == 10 ? fake_head_raw * 3u : 1219;
    return 0;
}
static uint32_t ms(void *x) {
    (void)x;
    return now_ms;
}
static void delay_us(void *x, unsigned us) {
    (void)x;
    if (!active_core) return;
    delay_remainder_us += us;
    while (delay_remainder_us >= 1000u) {
        delay_remainder_us -= 1000u;
        now_ms++;
        pb_step_tick(active_core);
    }
}
static int tx(void *x, const char *s, size_t n) {
    (void)x;
    if (n > sizeof(output) - used - 1)
        return 0;
    memcpy(output + used, s, n);
    used += n;
    output[used] = 0;
    return 1;
}
static void fail(unsigned line, const char *msg) {
    fprintf(stderr, "line %u: %s (pending output: %s)\n", line, msg, output);
    exit(1);
}
static void consume(unsigned line, const char *expect) {
    size_t n = strlen(expect);
    if (used < n || memcmp(output, expect, n))
        fail(line, "reply mismatch");
    if (used == n) {
        output[0] = 0;
        used = 0;
    } else {
        memmove(output, output + n, used - n);
        used -= n;
        output[used] = 0;
    }
}
int main(int argc, char **argv) {
    pb_core c;
    pb_hal h = {0, mode, wr, rd, adc, 0, ms, tx, delay_us, wr}; /* wr doubles as the fast writer */
    FILE *f;
    char line[2048];
    unsigned ln = 0;
    if (argc != 2) {
        fprintf(stderr, "usage: pb_test transcript\n");
        return 2;
    }
    f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    /* Sensor math must reject unset calibration and invalid divider voltages. */
    {
        unsigned value = 777u;
        if (!pb_sensor_temperature_calibrated() || !pb_sensor_battery_calibrated())
            fail(0, "sensor calibration constants are inconsistent");
        if (!pb_sensor_thermistor_ohms(1000u, 3300u, 56000u, &value) || value != 24347u)
            fail(0, "thermistor divider resistance conversion mismatch");
        value = 777u;
        if (pb_sensor_thermistor_ohms(3300u, 3300u, 56000u, &value) || value != 777u)
            fail(0, "invalid thermistor voltage was accepted or output modified");
        if (!pb_sensor_battery_mv(1250u, 4u, 1u, &value) || value != 5000u)
            fail(0, "battery divider conversion mismatch");
        value = 777u;
        if (pb_sensor_battery_mv(1250u, 0u, 1u, &value) || value != 777u)
            fail(0, "unset battery divider was accepted or output modified");
        {
            int temperature = 123456;
            if (!pb_sensor_temperature_mC(10000u, 10000u, 3950u, 25, &temperature) ||
                temperature < 24900 || temperature > 25100)
                fail(0, "NTC beta conversion at R0 does not match T0");
            temperature = 123456;
            if (pb_sensor_temperature_mC(10000u, 0u, 3950u, 25, &temperature) ||
                temperature != 123456)
                fail(0, "unset NTC R0 was accepted or output modified");
        }
    }
    /* Stock thermistor conversion must match the original firmware's table and evidence points. */
    {
        int t = -1, prev = 1000;
        unsigned raw;
        if (pb_therm_table[20] != 100000u || pb_therm_table[45] != 30000u)
            fail(0, "thermistor table anchor mismatch");
        if (!pb_sensor_head_temp_c(357u, &t) || t != 25)
            fail(0, "raw 357 should read 25 degC");
        if (pb_sensor_head_temp_c(900u, &t) || pb_sensor_head_temp_c(20u, &t))
            fail(0, "open/shorted sensor was accepted");
        for (raw = 21u; raw <= 899u; raw++) {
            if (!pb_sensor_head_temp_c(raw, &t) || t > prev)
                fail(0, "temperature is not monotonic in raw");
            prev = t;
        }
        raw = pb_sensor_hot_raw_limit(PB_TUNE_TEMP_CUTOFF_C);
        if (!raw || !pb_sensor_head_temp_c(raw, &t) || t < PB_TUNE_TEMP_CUTOFF_C ||
            (pb_sensor_head_temp_c(raw + 1u, &t) && t >= PB_TUNE_TEMP_CUTOFF_C))
            fail(0, "hot raw limit is not the boundary");
    }
    pb_init(&c, &h);
    active_core = &c;
    while (fgets(line, sizeof line, f)) {
        char *s, *e;
        ln++;
        s = line;
        while (*s && isspace((unsigned char)*s))
            s++;
        e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1]))
            *--e = 0;
        if (!*s || *s == '#')
            continue;
        if (*s == '<') {
            if (s[1] != ' ')
                fail(ln, "bad expected directive");
            char exp[512];
            snprintf(exp, sizeof exp, "%s", s + 2);
            char *p = strstr(exp, "{cfg}");
            if (p) {
                memmove(p + strlen(PB_CFG_HASH_STR), p + 5, strlen(p + 5) + 1);
                memcpy(p, PB_CFG_HASH_STR, strlen(PB_CFG_HASH_STR));
            }
            p = strstr(exp, "{int}");
            if (p) {
                char *q = strchr(output, '\n');
                char *a = strstr(output, "up=");
                if (!q || !a || a > q)
                    fail(ln, "missing up integer");
                char *d = a + 3;
                if (!isdigit((unsigned char)*d))
                    fail(ln, "bad up integer");
                while (isdigit((unsigned char)*d))
                    d++;
                if (strncmp(output, exp, (size_t)(p - exp)) || strncmp(d, p + 5, strlen(p + 5)))
                    fail(ln, "reply template mismatch");
                size_t usedline = (size_t)(q - output) + 1;
                memmove(output, output + usedline, used - usedline);
                used -= usedline;
                output[used] = 0;
                continue;
            }
            if (!strchr(exp, '\n'))
                strcat(exp, "\n");
            consume(ln, exp);
            continue;
        }
        if (*s == '>') {
            int cr = s[1] == 'r';
            char *cmd = s + (cr ? 3 : 2);
            if (s[1] != ' ' && !cr)
                fail(ln, "bad send directive");
            if (cr && s[2] != ' ')
                fail(ln, "bad CRLF directive");
            pb_rx(&c, (const unsigned char *)cmd, strlen(cmd));
            if (cr)
                pb_rx(&c, (const unsigned char *)"\r", 1);
            pb_rx(&c, (const unsigned char *)"\n", 1);
            continue;
        }
        if (*s == '@') {
            unsigned a, b;
            char pin[16], val[16], frame[521];
            if (sscanf(s, "@accept %u", &a) == 1) {
                if (a < 1 || a > 255)
                    fail(ln, "bad @accept id");
                pb_tbp_accept(&c, (uint8_t)a);
                continue;
            }
            if (sscanf(s, "@history %u %u", &a, &b) == 2) {
                if (a < 1 || a > 255 || c.tbp_history[a] != b)
                    fail(ln, "history state mismatch");
                continue;
            }
            if (sscanf(s, "@last-terminal-byte %u %u", &a, &b) == 2) {
                if (a >= c.tbp_last_terminal_len || c.tbp_last_terminal_body[a] != b)
                    fail(ln, "last terminal body byte mismatch");
                continue;
            }
            {
                unsigned phase, active, completed, remaining, pending;
                if (sscanf(s, "@scheduler phase=%u active=%u completed=%u remaining=%u pending=%u",
                           &phase, &active, &completed, &remaining, &pending) == 5) {
                    if (c.step_phase != phase || c.step_active != active || c.step_completed != completed ||
                        c.step_remaining != remaining || c.tbp_comp_count != pending)
                        fail(ln, "scheduler state mismatch");
                    continue;
                }
            }
            if (sscanf(s, "@time %u", &a) == 1) {
                now_ms = a;
                continue;
            }
            if (sscanf(s, "@advance %u", &a) == 1) {
                for (b = 0; b < a; b++) {
                    now_ms++;
                    pb_step_tick(&c);
                    if (now_ms % 10 == 0)
                        pb_poll(&c);
                }
                continue;
            }
            if (!strcmp(s, "@rx-overflow")) {
                c.llen = 0;
                c.discard = 1; /* mirror the app's ring-overflow resync state */
                continue;
            }
            if (sscanf(s, "@advance-no-step %u", &a) == 1) {
                for (b = 0; b < a; b++) {
                    now_ms++;
                    if (now_ms % 10 == 0) pb_poll(&c); /* simulate a dead step-timer ISR */
                }
                continue;
            }
            if (sscanf(s, "@poll-bound %u", &a) == 1) {
                uint32_t before = now_ms;
                pb_poll(&c);
                if ((uint32_t)(now_ms - before) > a)
                    fail(ln, "one executor poll exceeded the documented simulated blocking-delay budget");
                continue;
            }
            if (sscanf(s, "@heat-used %u", &a) == 1) { /* pretend the duty budget is partly spent right now */
                c.tbp_heat_used_us = a;
                c.tbp_heat_window_start = now_ms;
                continue;
            }
            if (!strcmp(s, "@dump")) { /* debug aid for authoring transcripts: print and discard pending output */
                printf("@@\n%s", output);
                used = 0;
                output[0] = 0;
                continue;
            }
            if (sscanf(s, "@adc head=%u", &a) == 1) { fake_head_raw = a; continue; }
            if (sscanf(s, "@adc vbat=%u", &a) == 1) { fake_vbat_mv = a; continue; }
            if (sscanf(s, "@adc fail=%u", &a) == 1) { fake_adc_fail = a; continue; }
            if (sscanf(s, "@pad %15[^=]=%15s", pin, val) == 2) {
                int i = pb_pin(pin);
                if (i < 0)
                    fail(ln, "unknown @pad pin");
                unsigned p = pb_defs[i].port, bit = pb_defs[i].bit;
                if (!strcmp(val, "1"))
                    pads[p] |= (uint16_t)(1u << bit);
                else if (!strcmp(val, "0"))
                    pads[p] &= (uint16_t)~(1u << bit);
                else
                    fail(ln, "bad @pad value");
                continue;
            }
            if (sscanf(s, "@hw %15s", pin) == 1) {
                char *q = strchr(s, ' ') + 1;
                while (*q) {
                    char name[16], state[16];
                    int n = 0;
                    if (sscanf(q, "%15[^=]=%15s%n", name, state, &n) != 2)
                        break;
                    int i = pb_pin(name);
                    if (i < 0)
                        fail(ln, "unknown @hw pin");
                    unsigned p = pb_defs[i].port, bit = pb_defs[i].bit;
                    unsigned m = (c.pins[i].mode);
                    if (!strcmp(state, "hi") && !(outs[p] & (1u << bit)))
                        fail(ln, "@hw expected hi");
                    if (!strcmp(state, "lo") && (outs[p] & (1u << bit)))
                        fail(ln, "@hw expected lo");
                    if (!strcmp(state, "in") && m != PB_IN && m != PB_UP && m != PB_DN &&
                        m != PB_HIZ)
                        fail(ln, "@hw expected input");
                    if (!strcmp(state, "hiz") && m != PB_HIZ)
                        fail(ln, "@hw expected high impedance");
                    if (!strcmp(state, "out") && m != PB_LO && m != PB_HI)
                        fail(ln, "@hw expected driven output");
                    q += n;
                    while (*q == ' ')
                        q++;
                }
                continue;
            }
            if (sscanf(s, "@writes %u", &a) == 1) {
                if (writes != a)
                    fail(ln, "write_port call count mismatch");
                writes = 0;
                continue;
            }
            if (sscanf(s, "@fast-estop-valid %u %520s", &a, frame) == 2) {
                uint8_t id = 0;
                if (!pb_tbp_fast_estop_line(frame, strlen(frame), &id) || id != a)
                    fail(ln, "fast ESTOP validation failed");
                continue;
            }
            if (sscanf(s, "@fast-estop-invalid %520s", frame) == 1) {
                if (pb_tbp_fast_estop_line(frame, strlen(frame), 0))
                    fail(ln, "invalid fast ESTOP frame accepted");
                continue;
            }
            if (!strcmp(s, "@disconnect")) {
                pb_tbp_cancel_work(&c, 4u);
                pb_safe_state(&c);
                c.tbp_active = 0;
                c.emergency_abort_requested = 1;
                c.step_active = 0;
                c.step_done_pending = 0;
                continue;
            }
            if (!strcmp(s, "@abort-fast")) {
                c.emergency_abort_requested = 1;
                c.step_active = 0;
                c.step_done_pending = 0;
                continue;
            }
            if (!strcmp(s, "@writes-reset")) {
                writes = 0;
                continue;
            }
            if (!strcmp(s, "@writes-zero")) {
                if (writes != 0)
                    fail(ln, "expected no write_port calls");
                continue;
            }
            if (!strncmp(s, "@reboot", 7)) {
                pb_init(&c, &h);
                continue;
            }
            if (!strncmp(s, "@config test", 12) || !strncmp(s, "@config real", 12))
                continue;
            if (!strncmp(s, "@send ", 6)) {
                char raw[512];
                if (sscanf(s + 6, "\"%511[^\"]\"", raw) != 1)
                    fail(ln, "bad @send");
                pb_rx(&c, (unsigned char *)raw, strlen(raw));
                continue;
            }
            if (!strncmp(s, "@embedded-cr", 12)) {
                const unsigned char raw[] = {'0','1','0','1','\r','G','G','\n'};
                pb_rx(&c, raw, sizeof raw);
                continue;
            }
            if (sscanf(s, "@long %u", &a) == 1) {
                char big[1024];
                if (a > 900)
                    fail(ln, "@long too large");
                memset(big, 'x', a);
                big[a] = '\n';
                pb_rx(&c, (unsigned char *)big, a + 1);
                continue;
            }
            fail(ln, "unknown directive");
        }
        fail(ln, "unknown transcript line");
    }
    fclose(f);
    if (used)
        fail(ln, "unexpected extra output");
    /* Local power button: boot hold must latch power on without being
       mistaken for a second hold, then a fresh one-second hold powers off. */
    {
        int bi, pi;
        unsigned bp, bb, pp, pb;
        memset(outs, 0, sizeof outs);
        memset(pads, 0, sizeof pads);
        now_ms = 0;
        pb_init(&c, &h);
        bi = pb_pin("PB1");
        pi = pb_pin("PA1");
        if (bi < 0 || pi < 0)
            fail(ln, "power control pins missing");
        bp = pb_defs[bi].port; bb = pb_defs[bi].bit;
        pp = pb_defs[pi].port; pb = pb_defs[pi].bit;
        outs[bp] &= (uint16_t)~(1u << bb); /* active-low button held */
        pb_power_led_poll(&c, 0);
        now_ms = 1000;
        pb_power_led_poll(&c, now_ms);
        if (!c.power_ready || !(outs[pp] & (1u << pb)))
            fail(ln, "one-second boot hold did not latch power on");
        now_ms = 1001;
        pb_power_led_poll(&c, now_ms);
        if (!c.power_ready || !(outs[pp] & (1u << pb)))
            fail(ln, "boot hold incorrectly triggered immediate shutdown");
        outs[bp] |= (uint16_t)(1u << bb); /* release before the next hold */
        now_ms = 1002;
        pb_power_led_poll(&c, now_ms);
        outs[bp] &= (uint16_t)~(1u << bb);
        now_ms = 1100;
        pb_power_led_poll(&c, now_ms);
        now_ms = 2100;
        pb_power_led_poll(&c, now_ms);
        if (c.power_ready || (outs[pp] & (1u << pb)))
            fail(ln, "one-second runtime hold did not cut power");
    }
    /* Boot wiggle checks: stable paper required, exact derived distance, pauses with coils held,
       and cancellation must stop motion. The outward physical direction remains owner-verified. */
    if (pb_pin("PA9") >= 0) {
        int bi = pb_pin("PB1"), pi = pb_pin("PB3"), run;
        unsigned bp, bb, pp, pb, i, k;
        if (bi < 0 || pi < 0) fail(ln, "power button or paper pin missing");
        bp = pb_defs[bi].port; bb = pb_defs[bi].bit;
        pp = pb_defs[pi].port; pb = pb_defs[pi].bit;
        if (PB_TUNE_BOOT_WIGGLE_MM != 5u ||
            PB_TUNE_BOOT_WIGGLE_STEPS != PB_TUNE_BOOT_WIGGLE_MM * PB_TUNE_STEPS_PER_MM ||
            PB_TUNE_BOOT_WIGGLE_STEPS != 160u)
            fail(ln, "boot wiggle is not derived from 5 mm and the configured calibration");
        for (run = 0; run < 4; run++) {
            unsigned seen_pause = 0, seen_reverse = 0, seen_release_pause = 0;
            unsigned pause_since = 0, release_since = 0, saw_any_wiggle = 0, saw_motion = 0;
            memset(outs, 0, sizeof outs);
            memset(pads, 0, sizeof pads);
            if (run == 2) pads[pp] |= (uint16_t)(1u << pb); /* no paper: PB3 high */
            now_ms = 0;
            pb_init(&c, &h);
            for (k = 0; k < 4; k++) {
                static const char *const mp[4] = {"PA9", "PA10", "PC3", "PC2"};
                if (c.pins[pb_pin(mp[k])].mode != PB_HIZ)
                    fail(ln, "stepper pins are not released at boot");
            }
            outs[bp] &= (uint16_t)~(1u << bb); /* hold the power button */
            for (i = 0; i < 4500; i++) {
                now_ms++;
                pb_step_tick(&c);
                if (run == 3 && now_ms == 1010u) pads[pp] |= (uint16_t)(1u << pb); /* paper disappears during debounce */
                if (now_ms % 10 == 0) pb_poll(&c);
                if (c.wiggle_stage) saw_any_wiggle = 1;
                if ((c.wiggle_stage >= 1u && c.wiggle_stage <= 4u) || c.step_active) saw_motion = 1;
                if (run == 1 && c.wiggle_stage == 1u && c.step_completed >= 20u) {
                    pb_safe_state(&c); /* cancel mid-outward leg */
                    break;
                }
                if (run != 0) continue;
                if (c.wiggle_stage == 2u && !seen_pause) {
                    seen_pause = 1;
                    pause_since = now_ms;
                    if (c.step_active || c.step_completed != PB_TUNE_BOOT_WIGGLE_STEPS)
                        fail(ln, "outward leg did not pause after exactly 5 mm");
                    for (k = 0; k < 4; k++) {
                        static const char *const mp[4] = {"PA9", "PA10", "PC3", "PC2"};
                        if (c.pins[pb_pin(mp[k])].mode == PB_HIZ)
                            fail(ln, "coils were released during the direction pause");
                    }
                }
                if (c.wiggle_stage == 3u && !seen_reverse) {
                    seen_reverse = 1;
                    if (!seen_pause || now_ms - pause_since < PB_TUNE_BOOT_WIGGLE_PAUSE_MS ||
                        c.step_reverse == PB_TUNE_BOOT_WIGGLE_OUT_REVERSE ||
                        c.step_completed != PB_TUNE_BOOT_WIGGLE_STEPS)
                        fail(ln, "reverse leg started without the configured pause or direction change");
                }
                if (c.wiggle_stage == 4u && !seen_release_pause) {
                    seen_release_pause = 1;
                    release_since = now_ms;
                    if (c.step_active || c.step_completed != 2u * PB_TUNE_BOOT_WIGGLE_STEPS)
                        fail(ln, "final pause did not begin after the return leg completed");
                    for (k = 0; k < 4; k++) {
                        static const char *const mp[4] = {"PA9", "PA10", "PC3", "PC2"};
                        if (c.pins[pb_pin(mp[k])].mode == PB_HIZ)
                            fail(ln, "coils were released before the final pause");
                    }
                }
            }
            if (run == 0) {
                if (c.wiggle_stage || c.step_active || !seen_pause || !seen_reverse || !seen_release_pause)
                    fail(ln, "boot wiggle did not complete every stage");
                if (c.step_completed != 2u * PB_TUNE_BOOT_WIGGLE_STEPS || c.step_phase != 0)
                    fail(ln, "boot wiggle was not net zero");
                if (now_ms - release_since < PB_TUNE_BOOT_WIGGLE_PAUSE_MS)
                    fail(ln, "coils released before the final pause elapsed");
            } else if (run == 1) {
                for (i = 0; i < 100; i++) {
                    now_ms++;
                    pb_step_tick(&c);
                    if (now_ms % 10 == 0) pb_poll(&c);
                }
                if (c.wiggle_stage || c.step_active || c.step_completed >= 40u)
                    fail(ln, "cancelled wiggle kept moving");
            } else if (run == 2) {
                if (saw_any_wiggle || c.wiggle_stage || c.step_active || c.step_completed)
                    fail(ln, "boot wiggle moved despite no paper");
            } else {
                if (saw_motion || c.wiggle_stage || c.step_active || c.step_completed)
                    fail(ln, "boot wiggle moved after paper disappeared during debounce");
            }
            for (k = 0; k < 4; k++) {
                static const char *const mp[4] = {"PA9", "PA10", "PC3", "PC2"};
                if (c.pins[pb_pin(mp[k])].mode != PB_HIZ)
                    fail(ln, "stepper coils left energized after boot wiggle handling");
            }
        }
    }
    printf("PASS %s\n", argv[1]);
    return 0;
}
