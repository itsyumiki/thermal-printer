/* Internal to pb_core.h; shared printer hardware primitives for the TBP executor. */
#ifndef PB_PRINTER_H
#define PB_PRINTER_H

/* Printer-specific commands and the nonblocking motor stepper. */
static const char *const pb_motor_pins[4] = {"PA9", "PA10", "PC3", "PC2"};
/* Official JieLi AC6956C half-step sequence on PA9, PA10, PC3, PC2.
 Owner confirms the phase pin mapping and motor function; phase timing/drive limits still require firmware-specific validation. */
static const uint8_t pb_motor_phases[8][4] = {{0, 1, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 1},
                                              {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0},
                                              {1, 0, 1, 0}, {0, 0, 1, 0}};

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

static inline void pb_step_tick(pb_core *c) {
    unsigned phase;
    if (!c->step_active || c->step_lock)
        return;
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
