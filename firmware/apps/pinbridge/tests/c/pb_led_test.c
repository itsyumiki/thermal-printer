/*
 * Power button / status LED behaviour (PB1 button, PA1 power latch, PC7 LED).
 *
 *  - The LED stays dark during the one-second startup hold and starts blinking only after it.
 *  - A host HELLO before the startup hold completes does not light the LED early.
 *  - HELLO after power-on still flashes twice and then holds the LED on.
 *  - Holding the button for one second after power-on cuts power and turns the LED dark for good
 *    (with and without a connected host); a new startup hold powers on and blinks again.
 *
 * Host-side simulation only (no firmware build, no hardware). From the repo root:
 *   cc -std=gnu11 -O1 -Wall -I firmware/apps/pinbridge/src -o /tmp/pb_led_test \
 *      firmware/apps/pinbridge/tests/c/pb_led_test.c && /tmp/pb_led_test
 */
#ifndef PB_PINS_HEADER
#define PB_PINS_HEADER "pb_pins.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/pb_core.h"

static unsigned now_ms, transitions, failures;
static uint16_t outs[4], pads[4];
static pb_core C;
static int button_pressed, pc7_prev;
static unsigned pc7_port, pc7_bit, pa1_port, pa1_bit, pb1_port, pb1_bit;

static int mode(void *x, unsigned p, unsigned b, unsigned m, unsigned l) {
    (void)x;
    if ((m == PB_LO || m == PB_HI) && l) outs[p] |= (uint16_t)(1u << b);
    else outs[p] &= (uint16_t)~(1u << b); /* inputs and pull-ups drive nothing */
    return 0;
}
static void wr(void *x, unsigned p, uint16_t mask, uint16_t v) {
    (void)x;
    outs[p] = (uint16_t)((outs[p] & ~mask) | (v & mask));
}
static uint16_t rd(void *x, unsigned p) {
    uint16_t v = (uint16_t)(outs[p] | pads[p]);
    (void)x;
    if (p == pb1_port) { /* active-low button with pull-up */
        v &= (uint16_t)~(1u << pb1_bit);
        if (!button_pressed) v |= (uint16_t)(1u << pb1_bit);
    }
    return v;
}
static int adc(void *x, int ch, unsigned *r, unsigned *m) {
    (void)x;
    if (r) *r = ch == -2 ? 500u : 412u;
    if (m) *m = ch == -2 ? 4000u : ch == 10 ? 1236u : 1219u;
    return 0;
}
static uint32_t ms(void *x) { (void)x; return now_ms; }
static void dly(void *x, unsigned us) { (void)x; (void)us; }
static int tx(void *x, const char *s, size_t n) { (void)x; (void)s; (void)n; return 1; }

static unsigned pc7(void) { return (outs[pc7_port] >> pc7_bit) & 1u; }
static unsigned pa1(void) { return (outs[pa1_port] >> pa1_bit) & 1u; }
static void step(unsigned n) {
    while (n--) {
        unsigned v;
        now_ms++;
        pb_step_tick(&C);
        if (now_ms % 10u == 0u) pb_poll(&C);
        v = pc7();
        if ((int)v != pc7_prev) { transitions++; pc7_prev = (int)v; }
    }
}
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL t=%u: %s\n", now_ms, msg); failures++; } } while (0)

static void begin(void) {
    pb_hal h = {0, mode, wr, rd, adc, 0, ms, tx, dly, wr, 0, 0, 0, 0};
    int i;
    memset(&C, 0, sizeof C); memset(outs, 0, sizeof outs); memset(pads, 0, sizeof pads);
    i = pb_pin("PC7"); pc7_port = pb_defs[i].port; pc7_bit = pb_defs[i].bit;
    i = pb_pin("PA1"); pa1_port = pb_defs[i].port; pa1_bit = pb_defs[i].bit;
    i = pb_pin("PB1"); pb1_port = pb_defs[i].port; pb1_bit = pb_defs[i].bit;
    now_ms = 1000; transitions = 0; button_pressed = 1; /* the button is held from power-up */
    pb_init(&C, &h);
    pc7_prev = (int)pc7();
}
static void send_hello(void) {
    uint8_t raw[8] = {1, 0x01, 1, 1};
    char hex[24];
    unsigned i;
    uint16_t crc = pb_tbp_crc(raw, 4u);
    raw[4] = (uint8_t)(crc >> 8); raw[5] = (uint8_t)crc;
    for (i = 0; i < 6u; i++) sprintf(hex + 2 * i, "%02X", raw[i]);
    strcat(hex, "\n");
    pb_rx(&C, (const unsigned char *)hex, strlen(hex));
}
static void wait_power_ready(void) {
    unsigned guard = 0;
    while (!C.power_ready && guard++ < 4000u) step(1);
}

static void startup_hold_then_blink(void) {
    begin();
    step(900);
    CHECK(transitions == 0 && pc7() == 0, "LED must stay dark during the startup hold");
    wait_power_ready();
    CHECK(C.power_ready && pa1() == 1, "the 1 s startup hold should latch power");
    CHECK(now_ms >= 2000u && now_ms <= 2100u, "power latches after about one second of holding");
    CHECK(transitions == 0, "the LED must not have blinked before the hold completed");
    step(450);
    CHECK(transitions == 0, "the first blink comes about 500 ms after the hold, not earlier");
    step(200);
    CHECK(transitions >= 1, "the LED starts blinking after the startup hold");
    step(2500);
    CHECK(transitions >= 5, "the LED keeps blinking while waiting for the host");
    send_hello();
    step(1000);
    CHECK(pc7() == 1 && C.led_mode == 2, "HELLO after power-on flashes then holds the LED on");
}
static void hello_during_hold_keeps_led_dark(void) {
    begin();
    step(500);
    send_hello();
    step(400);
    CHECK(C.tbp_active, "the HELLO session should be accepted");
    CHECK(transitions == 0 && pc7() == 0 && !C.power_ready, "HELLO before the startup hold must not light the LED");
    wait_power_ready();
    step(60);
    CHECK(C.power_ready && pc7() == 1 && C.led_mode == 2, "with a host already connected the LED goes steady on after the hold");
}

/* After power-on, release the button, optionally connect a host, then hold for the shutdown. */
static void shutdown_hold_goes_dark(int host_connected) {
    unsigned seen;
    begin();
    wait_power_ready();
    step(100);
    button_pressed = 0; /* a completed hold must be released before the next one */
    step(100);
    if (host_connected) send_hello();
    step(1500);
    CHECK(host_connected ? (pc7() == 1 && C.led_mode == 2) : C.led_mode == 0, "LED should be on or blinking before shutdown");
    button_pressed = 1;
    step(850);
    CHECK(C.power_ready && pa1() == 1, "shutdown must wait for the full one-second hold");
    step(300);
    CHECK(!C.power_ready && pa1() == 0, "holding the button for one second cuts power (PA1)");
    CHECK(pc7() == 0 && C.led_mode == 4, "the LED goes dark when the shutdown hold completes");
    button_pressed = 0;
    seen = transitions;
    step(5000);
    CHECK(transitions == seen && pc7() == 0, "the LED stays dark after shutdown (no blinking)");
    if (!host_connected) { /* a fresh startup hold works again after a shutdown */
        button_pressed = 1;
        wait_power_ready();
        CHECK(C.power_ready && pa1() == 1, "the startup hold powers on again after a shutdown");
        step(700);
        CHECK(transitions > seen, "the LED blinks again once powered on");
    }
}

int main(void) {
    startup_hold_then_blink();
    hello_during_hold_keeps_led_dark();
    shutdown_hold_goes_dark(0);
    shutdown_hold_goes_dark(1);
    printf("%s (%u failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
