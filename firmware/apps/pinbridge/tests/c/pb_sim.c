/* Host-side PTY simulator for exercising the real portable core through serial. */
#ifndef PB_PINS_HEADER
#define PB_PINS_HEADER "pb_pins.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/pb_core.h"

static uint32_t now_ms;
static uint16_t outputs[4], pads[4];

static int sim_mode(void *ctx, unsigned port, unsigned bit, unsigned mode, unsigned level) {
    (void)ctx;
    (void)mode;
    if (level)
        outputs[port] |= (uint16_t)(1u << bit);
    else
        outputs[port] &= (uint16_t)~(1u << bit);
    return 0;
}
static void sim_write(void *ctx, unsigned port, uint16_t mask, uint16_t value) {
    (void)ctx;
    outputs[port] = (uint16_t)((outputs[port] & ~mask) | (value & mask));
}
static uint16_t sim_read(void *ctx, unsigned port) {
    (void)ctx;
    return (uint16_t)(pads[port] | outputs[port]);
}
static int sim_adc(void *ctx, int channel, unsigned *raw, unsigned *mv) {
    (void)ctx;
    (void)channel;
    if (raw)
        *raw = 412;
    if (mv)
        *mv = 1219;
    return 0;
}
static uint32_t sim_millis(void *ctx) {
    (void)ctx;
    return now_ms;
}
static int sim_tx(void *ctx, const char *data, size_t length) {
    (void)ctx;
    if (fwrite(data, 1, length, stdout) != length)
        return 0;
    return fflush(stdout) == 0;
}

static void sim_delay_us(void *ctx, unsigned us) {
    (void)ctx;
    (void)us;
}

int main(void) {
    pb_core core;
    const pb_hal hal = {NULL, sim_mode,   sim_write, sim_read,    sim_adc,
                        NULL, sim_millis, sim_tx,    sim_delay_us, NULL};
    int ch;
    pb_init(&core, &hal);
    while ((ch = getchar()) != EOF) {
        unsigned char byte = (unsigned char)ch;
        ++now_ms;
        pb_rx(&core, &byte, 1);
        if (byte == '\n')
            pb_poll(&core);
    }
    return 0;
}
