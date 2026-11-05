/* pinbridge firmware adapter. [untested on hardware] */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "app_action.h"
#include "usb/device/usb_stack.h"
#include "usb/usb_config.h"
#include "usb/device/cdc.h"
#include "asm/adc_api.h"
#include "pb_core.h"
/* The BR23 SDK header omits this public implementation declaration. */
extern u32 adc_get_value(u32 ch);
extern void wdt_clear(void);
#if !TCFG_USB_SLAVE_CDC_ENABLE
#error "pinbridge requires USB CDC"
#endif
#define PB_USB 0
#define PB_RING 1024
#define PB_HEATER_DEADMAN_MARGIN_US 100u /* setup allowance; wrap ISR adds up to another 1 ms */
/* Plain-text heater trace (see the HL section below). 0 compiles it out. */
#ifndef PB_HEATER_TRACE
#define PB_HEATER_TRACE 1
#endif
static pb_core core;
static volatile u8 heater_timer_armed, timebase_ready, timebase_stalled;
static volatile u32 heater_deadman_at;
static volatile u32 tb_wraps, tb_isr_con; /* timebase wrap count and the CON value the wrap ISR writes back */
static volatile u32 tb_max_lag; /* largest TIMER3 CNT seen at ISR entry = worst ISR lateness in 1/6 us ticks */
static OS_SEM rx_sem, cmd_sem, tx_sem;
static volatile u8 detached, rx_overflow, estop_fast_requested, hello_fast_clear_requested, fast_estop_id;
static u8 ring[PB_RING], usb_rx_ring[PB_RING];
static volatile u16 wr, rd, usb_rx_wr, usb_rx_rd;
static char fast_line[521];
static unsigned fast_line_len;
static u8 fast_line_discard;
static JL_PORT_FLASH_TypeDef *ports[4] = {JL_PORTA, JL_PORTB, JL_PORTC, JL_PORTD};
static int hal_mode(void *ctx, unsigned port, unsigned bit, unsigned mode, unsigned level);
static void hal_heater_deadman_disarm(void *ctx);
/* HL trace hooks, defined after the timebase. Every hl_* that takes `_locked` must be called with IRQs masked. */
enum { HL_K_PIN, HL_K_PULSE_START, HL_K_PULSE_END, HL_K_ARM };
enum { HL_S_WRITE, HL_S_MODE, HL_S_DISARM, HL_S_ISR_CUT, HL_S_ARM };
#if PB_HEATER_TRACE
static void hl_pin_locked(u8 src, u32 old_out, u32 new_out, u8 in_isr);
static void hl_arm_locked(u32 usec, u32 deadline);
static void hl_flush(void);
#else
#define hl_pin_locked(src, o, n, i) ((void)0)
#define hl_arm_locked(u, d) ((void)0)
#define hl_flush() ((void)0)
#endif
static void pb_wakeup(void *dev) {
    (void)dev;
    os_sem_post(&rx_sem);
}
static unsigned rx_ring_pop(u8 *dst, unsigned cap) {
    unsigned n = 0;
    local_irq_disable();
    while (usb_rx_rd != usb_rx_wr && n < cap) {
        dst[n++] = usb_rx_ring[usb_rx_rd];
        usb_rx_rd = (u16)((usb_rx_rd + 1u) % PB_RING);
    }
    local_irq_enable();
    return n;
}
static void rx_ring_push(const u8 *src, unsigned count) {
    unsigned i;
    local_irq_disable();
    for (i = 0; i < count; i++) {
        u16 next = (u16)((usb_rx_wr + 1u) % PB_RING);
        if (next == usb_rx_rd) {
            rx_overflow = 1;
            break;
        }
        usb_rx_ring[usb_rx_wr] = src[i];
        usb_rx_wr = next;
    }
    local_irq_enable();
    os_sem_post(&cmd_sem);
}
static void request_fast_abort(u8 id) {
    int k;
    estop_fast_requested = 1;
    fast_estop_id = id;
    core.emergency_abort_requested = 1;
    hal_heater_deadman_disarm(0);
    k = pb_pin("PB11");
    if (k >= 0) (void)hal_mode(0, pb_defs[k].port, pb_defs[k].bit, PB_LO, 0);
}
static int fast_tbp_frame(const char *line, unsigned chars, u8 *raw, unsigned expected_bytes) {
    unsigned i;
    u16 crc;
    if (chars != expected_bytes * 2u) return 0;
    for (i = 0; i < expected_bytes; i++) {
        int hi = pb_tbp_hexval(line[i * 2u]);
        int lo = pb_tbp_hexval(line[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return 0;
        raw[i] = (u8)((hi << 4) | lo);
    }
    crc = pb_tbp_crc(raw, expected_bytes - 2u);
    return crc == (u16)(((u16)raw[expected_bytes - 2u] << 8) | raw[expected_bytes - 1u]);
}
static void fast_control_line(void) {
    u8 raw[6];
    unsigned len = fast_line_len;
    if (pb_tbp_fast_estop_line(fast_line, len, &raw[0])) {
        request_fast_abort(raw[0]);
    } else if (len && fast_line[len - 1u] == '\r') {
        len--;
        if (len == 12u && fast_tbp_frame(fast_line, len, raw, 6u) &&
               raw[1] == 0x01u && raw[2] == 1u && raw[3] == 1u) {
            hello_fast_clear_requested = 1;
        }
    } else if (len == 12u && fast_tbp_frame(fast_line, len, raw, 6u) &&
               raw[1] == 0x01u && raw[2] == 1u && raw[3] == 1u) {
        hello_fast_clear_requested = 1;
    }
}
static void fast_scan_bytes(const u8 *src, unsigned count) {
    unsigned i;
    for (i = 0; i < count; i++) {
        u8 ch = src[i];
        if (ch == '\n') {
            if (!fast_line_discard && fast_line_len) fast_control_line();
            fast_line_len = 0;
            fast_line_discard = 0;
        } else if (!fast_line_discard) {
            if (fast_line_len < sizeof fast_line) fast_line[fast_line_len++] = (char)ch;
            else { fast_line_len = 0; fast_line_discard = 1; }
        }
    }
}
static void force_estop_after_rx_overflow(void) {
    static const char hx[] = "0123456789ABCDEF";
    u8 raw[5];
    char line[11];
    u16 crc;
    unsigned i;
    if (!estop_fast_requested || detached || core.tbp_stopped) return;
    raw[0] = fast_estop_id;
    raw[1] = 0xFFu;
    raw[2] = 0;
    crc = pb_tbp_crc(raw, 3u);
    raw[3] = (u8)(crc >> 8);
    raw[4] = (u8)crc;
    for (i = 0; i < sizeof raw; i++) {
        line[i * 2u] = hx[raw[i] >> 4];
        line[i * 2u + 1u] = hx[raw[i] & 15u];
    }
    line[10] = 0;
    (void)pb_tbp_line(&core, line);
}
static void rx_task(void *priv) {
    u8 b[64];
    (void)priv;
    for (;;) {
        os_sem_pend(&rx_sem, 1);
        if (detached) {
            fast_line_len = 0;
            fast_line_discard = 0;
            os_sem_post(&cmd_sem);
            continue;
        }
        for (unsigned nread = 0; nread < 32; nread++) {
            u32 got = usb_g_bulk_read(PB_USB, CDC_DATA_EP_OUT, b, sizeof b, 0);
            if (!got) break;
            fast_scan_bytes(b, got);
            rx_ring_push(b, got);
        }
    }
}
static int hal_mode(void *ctx, unsigned port, unsigned bit, unsigned mode, unsigned level) {
    JL_PORT_FLASH_TypeDef *p = ports[port];
    u32 mask = 1u << bit;
    u32 old_out;
    (void)ctx;
    local_irq_disable();
    old_out = p->OUT;
    if (mode == PB_HIZ) {
        p->DIR |= mask;
        p->PU &= ~mask;
        p->PD &= ~mask;
        p->DIE &= ~mask;
        p->DIEH &= ~mask;
    } else if (mode == PB_IN || mode == PB_UP || mode == PB_DN || mode == PB_ADC) {
        p->DIR |= mask;
        p->PU = (p->PU & ~mask) | (mode == PB_UP ? mask : 0);
        p->PD = (p->PD & ~mask) | (mode == PB_DN ? mask : 0);
        p->DIE = (p->DIE & ~mask) | ((mode == PB_ADC) ? 0 : mask);
        p->DIEH &= ~mask;
    } else if (mode == PB_LO || mode == PB_HI) {
        if (level)
            p->OUT |= mask;
        else
            p->OUT &= ~mask;
        p->DIE |= mask;
        p->DIR &= ~mask;
    } else {
        local_irq_enable();
        return -1;
    }
    if (port == 1 && bit == 11) hl_pin_locked(HL_S_MODE, old_out, p->OUT, 0);
    local_irq_enable();
    return 0;
}
static void hal_write(void *ctx, unsigned port, uint16_t mask, uint16_t vals) {
    u32 old_out;
    (void)ctx;
    local_irq_disable();
    old_out = ports[port]->OUT;
    ports[port]->OUT = (old_out & ~mask) | (vals & mask);
    if (port == 1 && (mask & (1u << 11))) hl_pin_locked(HL_S_WRITE, old_out, ports[port]->OUT, 0);
    local_irq_enable();
}
static void hal_write_fast(void *ctx, unsigned port, uint16_t mask, uint16_t vals) {
    (void)ctx;
    if (port == 1 && (mask & (1u << 11))) { /* rare: shift/clock writes never touch PB11 */
        hal_write(ctx, port, mask, vals);
        return;
    }
    ports[port]->OUT = (ports[port]->OUT & ~mask) | (vals & mask);
}
static uint16_t hal_read(void *ctx, unsigned port) {
    (void)ctx;
    return (uint16_t)ports[port]->IN;
}
static u16 adc_registered;
static u32 adc_ready_at[16];
static int hal_adc(void *ctx, int ch, unsigned *raw, unsigned *mv) {
    u32 sdk_ch, now;
    (void)ctx;
    if (ch == -2) {
        if (!raw && !mv)
            return 0;
        if (raw)
            *raw = adc_get_value(AD_CH_VBAT);
        if (mv)
            *mv = adc_get_voltage(AD_CH_VBAT) *
                  4u; /* SDK VBAT channel measures one quarter of the rail */
        return 0;
    }
    if (ch < 0 || ch >= 16)
        return -1;
    sdk_ch = (u32)ch;
    if (!(adc_registered & (1u << sdk_ch))) {
        adc_add_sample_ch(sdk_ch);
        adc_registered |= (u16)(1u << sdk_ch);
        adc_ready_at[sdk_ch] =
            jiffies + 10u; /* let the SDK's 2 ms scan loop collect a real sample */
    }
    if (!raw && !mv)
        return 0; /* capability/probe call from MOD */
    now = jiffies;
    if ((int32_t)(now - adc_ready_at[sdk_ch]) < 0)
        return -1;
    if (raw)
        *raw = adc_get_value(sdk_ch);
    if (mv)
        *mv = adc_get_voltage(sdk_ch);
    return 0;
}
static uint32_t hal_ms(void *ctx) {
    (void)ctx;
    return jiffies * 10u;
}
/* ---- Timebase (Sprint 2) ------------------------------------------------------------------------------
 * TIMER3 is a free-running microsecond timebase: 24 MHz oscillator / 4 = 6 ticks per us, PRD 6000 = one wrap
 * (and one ISR) every 1 ms exactly. Sprint 2/3 used 10 ms; it is 1 ms now because the wrap ISR is also the
 * STEPPER TICK (pb_step_tick runs from it while a motion is active), replacing the SDK sys_hi_timer, whose
 * tick is a fixed 2.000 ms (TIMER1, measured) and capped feed speed at 15.625 mm/s. Costs: the ISR is about
 * 10x as frequent (roughly 1 % of the CPU, not measured) and IRQs must never stay masked for 1 ms or more
 * (one late wrap is covered by the pending flag, two are lost and the clock steps back by 1 ms). tb_max_lag
 * records the worst ISR lateness, in 1/6 us ticks (6000 = a full period), so that margin is visible in the HL
 * trace. The heater deadman now trips within about 1 ms of its deadline instead of 10 ms. /4 stays the finest
 * resolution (1/6 us) that fits a whole number of microseconds into PRD. The prescale index comes from the
 * SDK irflt.c table (1 = /4); Sprint 1 measured the oscillator at 23.97-24.00 MHz for every prescaler.
 * CON: [3:2]=0b10 oscillator, [7:4]=1 prescale /4, BIT(0) plain periodic timer mode, BIT(14) clear pending.
 * Do NOT set BIT(1): that is the IR-capture mode (bench 2026-11-04).
 * The read algorithm and the delay loop live in pb_timebase.h so the C harness tests the same code. Open
 * assumption, checked by the TIMEBASE self-test on hardware: the counter reloads by itself at PRD.
 * TIMER0/2 are SDK timers and TIMER1 belongs to sys_hi_timer, so TIMER3 is the only one this app can take. */
#define PB_TB_TIMER JL_TIMER3
#define PB_TB_IRQ IRQ_TIME3_IDX
#define PB_TB_CON ((0b10u << 2) | (1u << 4) | BIT(0) | BIT(14))
#define PB_TB_WRAPS() (tb_wraps)
#define PB_TB_CNT() ((u32)(PB_TB_TIMER->CNT & 0xffffu))
#define PB_TB_PEND() (PB_TB_TIMER->CON & BIT(15))
#include "pb_timebase.h"
___interrupt
void pb_timebase_isr(void) {
    u32 cnt = PB_TB_TIMER->CNT & 0xffffu; /* the counter keeps running after the wrap: this is how late we are */
    u8 normal = tb_isr_con == PB_TB_CON; /* false while TIMETEST borrows the timer */
    tb_wraps++; /* before the pending flag is cleared: pb_tb_us() relies on this order */
    core.heater_isr_count++; /* every TIMER3 ISR, kept for the TIMETEST TIMER3 rows */
    if (normal && cnt > tb_max_lag) tb_max_lag = cnt;
    if (pb_heater_deadman_expire(&heater_timer_armed,
            (tb_wraps * PB_TB_PERIOD_US) + (cnt / PB_TB_TICKS_PER_US),
            heater_deadman_at, &core.heater_deadman_trips)) {
        /* TIMER3 wraps every 1 ms: the main loop normally ends the pulse at its exact
         * deadline plus the 100 us setup allowance; this independent cutoff is bounded by one
         * additional wrap period after that margin. */
        u32 old_out = ports[1]->OUT;
        ports[1]->OUT = old_out & ~(1u << 11);
        hl_pin_locked(HL_S_ISR_CUT, old_out, ports[1]->OUT, 1); /* IRQs are masked inside the ISR */
    }
    /* Stepper tick: one pb_step_tick per wrap = per 1 ms, only while a motion is active and TIMETEST is not
     * running. An ESTOP request drops the motion here, as the old SDK-timer callback did. */
    if (normal && core.step_active) {
        core.step_timer_callbacks++;
        if (estop_fast_requested) {
            core.step_active = 0;
            core.step_done_pending = 0;
        } else {
            pb_step_tick(&core);
        }
    }
    PB_TB_TIMER->CON = tb_isr_con; /* PB_TB_CON: clear pending and keep counting (TIMETEST swaps in BIT(14): stop) */
}
/* (Re)start the timebase from zero with the given wrap count. Task context only (local_irq_* do not nest). */
static void pb_timebase_start(u32 wraps) {
    local_irq_disable();
    PB_TB_TIMER->CON = BIT(14);
    PB_TB_TIMER->CNT = 0;
    PB_TB_TIMER->PRD = PB_TB_PRD;
    tb_wraps = wraps;
    tb_isr_con = PB_TB_CON;
    PB_TB_TIMER->CON = PB_TB_CON;
    local_irq_enable();
}
static void pb_timebase_init(void) {
    PB_TB_TIMER->CON = BIT(14);
    heater_timer_armed = 0;
    request_irq(PB_TB_IRQ, 5, pb_timebase_isr, 0);
    /* Explicitly unmask the IRQ source; without it the wrap count never advances. */
    irq_unmask_set(PB_TB_IRQ, 0);
    pb_timebase_start(0);
    timebase_ready = 1;
}
static uint32_t hal_micros(void *ctx) {
    (void)ctx;
    if (!timebase_ready || timebase_stalled) return (uint32_t)jiffies * 10000u; /* 10 ms steps, only if the timer failed */
    return pb_tb_us();
}
/* Old software loop, calibrated on hardware (2026-11-02): about 3.5 us fixed plus 0.85 us per iteration, and
 * bimodal between runs. Only used when the timebase is not running or has stalled. */
static void soft_delay_us(unsigned us) {
    volatile unsigned n;
    unsigned loops = (us * 11u + 5u) / 10u;
    for (n = 0; n < loops; n++) {
        __asm__ volatile("nop");
    }
}
/* Waits on the timebase, so the delay is real time to within one read (the per-call floor is the cost of
 * pb_tb_us(), measured by the DELAY2 self-test row). If the timebase is not running, or stops advancing,
 * the bounded software loop finishes the delay and the timebase is not trusted again until reboot. */
static void hal_delay_us(void *ctx, unsigned us) {
    (void)ctx;
    if (!us) return;
    if (timebase_ready && !timebase_stalled) {
        if (pb_tb_delay_us(us) == 0) return;
        timebase_stalled = 1;
        core.timebase_stalls++;
    }
    soft_delay_us(us);
}
static int hal_tx(void *ctx, const char *s, size_t n) {
    size_t i;
    u16 free_space;
    (void)ctx;
    free_space = (u16)((rd + PB_RING - wr - 1) % PB_RING);
    if (n > free_space)
        return -1;
    for (i = 0; i < n; i++) {
        ring[wr] = (u8)s[i];
        wr = (u16)((wr + 1) % PB_RING);
    }
    os_sem_post(&tx_sem);
    return (int)n;
}
static void tx_task(void *priv) {
    u8 b[64];
    (void)priv;
    for (;;) {
        unsigned n = 0;
        while (rd != wr && n < sizeof b) {
            b[n++] = ring[rd];
            rd = (u16)((rd + 1) % PB_RING);
        }
        if (n) {
            (void)cdc_write_data(PB_USB, b, n);
        } else
            os_sem_pend(&tx_sem, 1);
    }
}
/* The core owns PB11's normal deadline. This independent wrap-ISR deadman is armed before
 * PB11 rises and cuts it low if the executor wedges. The 100 us setup allowance plus the
 * 10 ms TIMER3 wrap bounds the cutoff to about 10.1 ms after the requested deadline; this is
 * not a precision timer. */
static int hal_heater_deadman_arm(void *ctx, unsigned usec) {
    (void)ctx;
    if (!timebase_ready || timebase_stalled || !usec || usec > PB_TUNE_HEATER_SLICE_MAX_US) return -1;
    local_irq_disable();
    heater_deadman_at = pb_tb_us() + usec + PB_HEATER_DEADMAN_MARGIN_US;
    heater_timer_armed = 1;
    hl_arm_locked(usec, heater_deadman_at);
    local_irq_enable();
    return 0;
}
static void hal_heater_deadman_disarm(void *ctx) {
    u32 old_out;
    (void)ctx;
    local_irq_disable();
    old_out = ports[1]->OUT;
    ports[1]->OUT = old_out & ~(1u << 11);
    heater_timer_armed = 0;
    hl_pin_locked(HL_S_DISARM, old_out, ports[1]->OUT, 0);
    local_irq_enable();
}
#if PB_HEATER_TRACE
/* ---- HL: plain-text heater (PB11) trace -----------------------------------------------------------------
 * Every touch of PB11 (HAL write/mode, deadman disarm, the TIMER3 ISR cut) and the executor's pulse start/end
 * are recorded in a small RAM ring together with the TIMER3 state, then printed as plain text lines starting
 * with "HL " from the command task (hl_flush, after pb_poll), never from the place that toggles the pin, so
 * the trace does not stretch the pulse. The lines go through the normal CDC tx ring but are NOT TBP frames
 * (the host shows them as non-TBP lines; the Bun host prints "HL " lines to stdout). They are dropped, and
 * counted, rather than crowd out TBP replies: a line is only sent while 160 bytes of the ring stay free.
 * Writes that leave PB11 unchanged are not logged (counted into `same=`), except a re-write of PB11 high.
 * Line fields: t=timebase us, src, PB11 old->new (OUT latch), in= pin read-back (may lag the write by a cycle),
 * dir=DIR bit (0 = output), armed/dl_left= deadman state and the us left to its deadline, cnt/con/wraps =
 * TIMER3 counter, CON and wrap count at that moment. On a fall (PB11 1->0) and on pulse_end the line also has
 * hi_us= how long PB11 stayed high by the timebase and hi_jiffy_ms= the same interval on the independent
 * 10 ms jiffy clock, starts/trips/over = core heater counters, lag = worst TIMER3 ISR lateness (1/6 us
 * ticks; 6000 = a whole period) and stp = stepper active. pulse_start/pulse_end come from the executor and
 * carry the requested width (req_us) against what its own loop measured (loop_us). */
typedef struct {
    u32 us, wraps, dl, a, b, c, d;
    u16 cnt, con, lag, pad;
    u8 kind, src, old, nw, in, dir, armed, stp;
} hl_ev;
#define HL_N 40u
#define HL_RESERVE 160u
static hl_ev hl_ring[HL_N];
static volatile u32 hl_wr, hl_rd, hl_lost, hl_same, hl_seq;
static u32 hl_rise_us, hl_rise_ms;
static hl_ev *hl_begin(u8 kind, u8 src, u8 in_isr) {
    u32 next = (hl_wr + 1u) % HL_N;
    hl_ev *e;
    if (next == hl_rd) {
        hl_lost++;
        return 0;
    }
    e = &hl_ring[hl_wr];
    e->kind = kind;
    e->src = src;
    e->cnt = (u16)(PB_TB_TIMER->CNT & 0xffffu);
    e->con = (u16)(PB_TB_TIMER->CON & 0xffffu);
    e->wraps = tb_wraps;
    /* Inside the wrap ISR tb_wraps is already bumped while the pending flag is still set, so pb_tb_us() would
     * count that wrap twice: build the time the same way the ISR deadman does. */
    e->us = in_isr ? tb_wraps * PB_TB_PERIOD_US + e->cnt / PB_TB_TICKS_PER_US
                   : (timebase_ready ? pb_tb_us() : (u32)jiffies * 10000u);
    e->dl = heater_deadman_at;
    e->armed = heater_timer_armed;
    e->lag = (u16)tb_max_lag;
    e->stp = core.step_active ? 1u : 0u;
    e->in = (u8)((ports[1]->IN >> 11) & 1u);
    e->dir = (u8)((ports[1]->DIR >> 11) & 1u);
    e->old = e->nw = 0;
    e->a = e->b = e->c = e->d = 0;
    e->pad = 0;
    return e;
}
static void hl_commit(void) {
    hl_wr = (hl_wr + 1u) % HL_N;
}
static void hl_pin_locked(u8 src, u32 old_out, u32 new_out, u8 in_isr) {
    u8 o = (u8)((old_out >> 11) & 1u), n = (u8)((new_out >> 11) & 1u);
    hl_ev *e;
    if (o == n && !n) {
        hl_same++;
        return;
    }
    e = hl_begin(HL_K_PIN, src, in_isr);
    if (!e) return;
    e->old = o;
    e->nw = n;
    e->a = core.heater_pulse_starts;
    e->b = core.heater_deadman_trips;
    e->c = core.heater_over_deadline;
    if (n && !o) {
        hl_rise_us = e->us;
        hl_rise_ms = (u32)jiffies * 10u;
    } else if (o && !n) {
        e->d = e->us - hl_rise_us; /* PB11 high time on the timebase, us */
        e->pad = (u16)(((u32)jiffies * 10u - hl_rise_ms) & 0xffffu); /* same interval on the jiffy clock, ms */
    }
    hl_commit();
}
static void hl_arm_locked(u32 usec, u32 deadline) {
    hl_ev *e = hl_begin(HL_K_ARM, HL_S_ARM, 0);
    if (!e) return;
    e->a = usec;
    e->b = deadline;
    hl_commit();
}
/* Called by the core (task context, IRQs enabled) around each heater slice. */
static void hal_heater_trace(void *ctx, unsigned kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    hl_ev *e;
    (void)ctx;
    if (kind != 1u && kind != 2u) return;
    local_irq_disable();
    e = hl_begin(kind == 1u ? HL_K_PULSE_START : HL_K_PULSE_END, 0, 0);
    if (e) {
        e->a = a;
        e->b = b;
        e->c = c;
        e->d = d;
        hl_commit();
    }
    local_irq_enable();
}
static char hl_buf[240];
static unsigned hl_len;
static void hl_s(const char *s) {
    while (*s && hl_len < sizeof hl_buf - 2u) hl_buf[hl_len++] = *s++;
}
static void hl_u(u32 v) {
    char t[11];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n && hl_len < sizeof hl_buf - 2u) hl_buf[hl_len++] = t[--n];
}
static void hl_i(s32 v) {
    if (v < 0) {
        hl_s("-");
        hl_u((u32)(-(v + 1)) + 1u);
    } else {
        hl_u((u32)v);
    }
}
static void hl_x(u32 v) {
    static const char hx[] = "0123456789abcdef";
    unsigned sh;
    hl_s("0x");
    for (sh = 16; sh; sh -= 4u)
        if (hl_len < sizeof hl_buf - 2u) hl_buf[hl_len++] = hx[(v >> (sh - 4u)) & 15u];
}
static void hl_kv(const char *k, u32 v) {
    hl_s(" ");
    hl_s(k);
    hl_s("=");
    hl_u(v);
}
static const char *const hl_src_name[] = {"write", "mode", "disarm", "isr_cut", "arm"};
static void hl_format(const hl_ev *e) {
    u8 full = 0; /* the longer tail (lateness, stepper, counters) only where a pulse ends: keeps the line rate down */
    hl_len = 0;
    hl_s("HL ");
    hl_u(hl_seq++);
    hl_kv("t", e->us);
    if (e->kind == HL_K_PIN) {
        hl_s(" PB11 ");
        hl_s(hl_src_name[e->src < 5u ? e->src : 0u]);
        hl_s(" ");
        hl_u(e->old);
        hl_s("->");
        hl_u(e->nw);
        hl_kv("in", e->in);
        hl_kv("dir", e->dir);
        if (e->old && !e->nw) {
            full = 1;
            hl_kv("hi_us", e->d);
            hl_kv("hi_jiffy_ms", e->pad);
            hl_kv("starts", e->a);
            hl_kv("trips", e->b);
            hl_kv("over", e->c);
        }
    } else if (e->kind == HL_K_ARM) {
        hl_s(" arm");
        hl_kv("req_us", e->a);
        hl_kv("deadline", e->b);
    } else if (e->kind == HL_K_PULSE_START) {
        hl_s(" pulse_start");
        hl_kv("req_us", e->a);
        hl_s(" slice=");
        hl_u(e->b);
        hl_s("/");
        hl_u(e->c);
        hl_kv("row_us", e->d);
        hl_kv("in", e->in);
    } else {
        full = 1;
        hl_s(" pulse_end");
        hl_kv("req_us", e->a);
        hl_kv("loop_us", e->b);
        hl_kv("loop_jiffy_ms", e->c);
        hl_kv("flags", e->d); /* bit0 = over deadline, bit1 = abort */
        hl_kv("in", e->in);
    }
    hl_kv("armed", e->armed);
    hl_s(" dl_left=");
    hl_i((s32)(e->dl - e->us));
    hl_kv("cnt", e->cnt);
    hl_s(" con=");
    hl_x(e->con);
    hl_kv("wraps", e->wraps);
    if (full) {
        hl_kv("lag", e->lag);
        hl_kv("stp", e->stp);
    }
    if (hl_same) {
        hl_kv("same", hl_same);
        hl_same = 0;
    }
    if (hl_lost) {
        hl_kv("lost", hl_lost);
        hl_lost = 0;
    }
    hl_s("\n");
}
/* Command task only (it owns the tx ring producer side together with pb_tx). */
static void hl_flush(void) {
    unsigned sent = 0;
    if (detached) return;
    while (hl_rd != hl_wr && sent < 12u) {
        u16 free_space = (u16)((rd + PB_RING - wr - 1) % PB_RING);
        hl_ev e;
        if (free_space < HL_RESERVE + sizeof hl_buf) break; /* keep room for TBP replies; retry next pass */
        e = hl_ring[hl_rd];
        hl_rd = (hl_rd + 1u) % HL_N;
        hl_format(&e);
        (void)hal_tx(0, hl_buf, hl_len);
        sent++;
    }
}
#endif /* PB_HEATER_TRACE */
/* ---- TIMETEST (opcode F0): timer self-test ------------------------------------------------------
 * Each source counts roughly `ms` milliseconds in its OWN units (compare periods, callbacks, jiffies, delay
 * calls). The RUNNING frame goes straight to USB just before counting starts and the DONE frame straight
 * after it ends, so the host's receive timestamps bracket the interval. wall / want shows how far the timer is
 * from real time; want is what the firmware assumes the count is worth. Nothing here touches the printer head,
 * motor or PB11.
 * Both frames bypass the tx ring on purpose (core.tx_direct_on): tx_task has a lower priority than the command
 * task and cannot run while this function busy-waits. cdc_write_data is mutex-protected, so it is safe next to
 * tx_task. The command task is stalled for the whole run; the core refuses TIMETEST unless the printer is idle.
 * A CRC-valid ESTOP still cuts the heater in the USB RX task; the loops below see estop_fast_requested and stop. */
/* PT_STEP times the SDK sys_hi_timer (TIMER1, 2.000 ms). The stepper no longer uses it: it runs from the TIMER3
 * wrap ISR (1 ms). The row is kept as the reference for the old tick. */
enum { PT_T3D4, PT_T3D16, PT_T3D64, PT_T3D256, PT_STEP, PT_JIFFY, PT_DELAY, PT_DELAY2, PT_TB };
/* TIMER3 CON[7:4] prescale index (SDK irflt.c table: 1=/4, 2=/16, 3=/64, 8=/256) and a PRD that keeps one compare
 * period at 10 / 40 / 100 / 500 ms when the oscillator really is 24 MHz. */
static const struct {
    u8 idx;
    u16 div, prd;
} pt_t3[4] = {{1, 4, 60000}, {2, 16, 60000}, {3, 64, 37500}, {8, 256, 46875}};
static volatile u32 pt_step_calls;
static void pt_step_cb(void *priv) {
    (void)priv;
    pt_step_calls++;
}
static int hal_tx_direct(void *ctx, const char *s, size_t n) {
    (void)ctx;
    return cdc_write_data(PB_USB, (u8 *)s, n) == n ? (int)n : -1;
}
static int hal_time_test(void *ctx, unsigned src, unsigned ms, pb_time_result *out) {
    u32 want_us = 0, n = 0, target = 0, period_us = 0, ticks = 0, isr_n = 0, dj = 0, i;
    unsigned long j0 = 0, last_j = 0, guard = 0, spin = 0;
    u16 id = 0;
    u8 t3 = src <= PT_T3D256;
    u32 con_run = 0, saved_wraps = 0;
    int rc = PB_TIME_OK;
    (void)ctx;
    if (src > PT_TB) return PB_TIME_SETUP;
    if (estop_fast_requested || detached) return PB_TIME_ABORTED;
    if (t3 && (!timebase_ready || heater_timer_armed)) return PB_TIME_SETUP;
    if (src == PT_TB && (!timebase_ready || timebase_stalled)) return PB_TIME_SETUP;
    if (src == PT_STEP && core.step_active) return PB_TIME_SETUP;
    /* Let queued output (the ACK) leave first so it cannot interleave with the direct frames below. */
    for (i = 0; rd != wr && i < 50; i++) os_time_dly(1);
    if (rd != wr) return PB_TIME_SETUP;
    /* Setup that can fail happens before RUNNING is sent. */
    if (t3) {
        period_us = (u32)pt_t3[src].prd * pt_t3[src].div / 24u; /* at 24 MHz */
        target = ((u32)ms * 1000u + period_us / 2u) / period_us;
        if (!target) target = 1;
        want_us = target * period_us;
        guard = period_us / 1000u * 3u + 100u; /* jiffies without a compare before giving up */
        con_run = (0b10u << 2) | ((u32)pt_t3[src].idx << 4) | BIT(0) | BIT(14);
    } else if (src == PT_STEP) {
        target = ms;
        want_us = ms * 1000u;
        guard = ms + 500u; /* jiffies */
        pt_step_calls = 0;
        id = sys_hi_timer_add(NULL, pt_step_cb, 1);
        if (!id) return PB_TIME_SETUP;
    } else if (src == PT_JIFFY) {
        target = ms / 10u; /* assumes 10 ms per jiffy (HZ 100); wall time shows the real value */
        want_us = target * 10000u;
        spin = (unsigned long)ms * 60000ul;
        j0 = jiffies; /* start on a jiffy edge */
        while (jiffies == j0 && spin) spin--;
        if (!spin) return PB_TIME_SETUP;
    } else if (src == PT_DELAY) {
        target = ms;
        want_us = ms * 1000u;
    } else if (src == PT_TB) {
        target = ms * 1000u; /* microseconds of timebase time; n reports what the timebase says passed */
        want_us = target;
        guard = ms / 10u * 2u + 100u; /* jiffies (an independent clock) before giving up */
    } else {
        target = ms * 500u;
        want_us = target * 2u;
    }
    if (t3) {
        /* TIMER3 is the timebase; borrow it for this run. The ISR now stops the timer on every compare (BIT(14)),
         * as the old heater ISR did, and the timebase is restarted from the saved wrap count afterwards, so
         * micros() loses the length of the run. Nothing else uses micros() while TIMETEST blocks the command task. */
        local_irq_disable();
        saved_wraps = tb_wraps;
        tb_isr_con = BIT(14);
        PB_TB_TIMER->CON = BIT(14);
        PB_TB_TIMER->CNT = 0;
        local_irq_enable();
    }
    core.tx_direct_on = 1;
    pb_tbp_status(&core, core.tbp_last_id, 2u, 0, 0); /* RUNNING: the host's start timestamp */
    core.tx_direct_on = 0;
    /* ---- counting: nothing allocates or sends from here to the end of the run ---- */
    j0 = jiffies;
    last_j = j0;
    if (t3) {
        /* The TIMER3 ISR stays enabled, and it is the ONLY thing counted: one compare = one ISR = one period.
         * The ISR (tb_isr_con = BIT(14)) stops the timer and clears the pending flag, bumping heater_isr_count; the loop sees the
         * bump, counts it once and restarts the timer from zero. Do NOT also count the pending flag
         * (CON bit 15): a compare can be seen there before the ISR runs, and the ISR then still runs
         * and counts the same compare again (the 2026-11-05 /4 /16 /64 rows were wrong for this reason).
         * Bias: every restart adds the ISR entry plus loop latency to its period, so n periods read a
         * little long (not measured; the 2 % gate allows 200 us per 10 ms period at /4). If the compare
         * IRQ never fires the run ends in PB_TIME_TIMEOUT instead of a wrong count. */
        u32 isr0 = core.heater_isr_count, k;
        PB_TB_TIMER->CON = BIT(14);
        PB_TB_TIMER->CNT = 0;
        PB_TB_TIMER->PRD = pt_t3[src].prd;
        PB_TB_TIMER->CON = con_run;
        while (n < target) {
            k = core.heater_isr_count - isr0;
            if (k) {
                isr0 += k;
                isr_n += k;
                n += k;
                PB_TB_TIMER->CON = BIT(14);
                PB_TB_TIMER->CNT = 0;
                PB_TB_TIMER->CON = con_run;
                last_j = jiffies;
                wdt_clear();
            } else if (estop_fast_requested) {
                rc = PB_TIME_ABORTED;
                break;
            } else if ((unsigned long)(jiffies - last_j) > guard) {
                rc = PB_TIME_TIMEOUT;
                break;
            } else {
                wdt_clear(); /* a dead compare IRQ must end in a timeout, not in a watchdog reset */
            }
        }
        dj = (u32)(jiffies - j0);
        PB_TB_TIMER->CON = BIT(14);
        PB_TB_TIMER->CNT = 0;
        ticks = n * pt_t3[src].prd;
    } else if (src == PT_STEP) {
        pt_step_calls = 0;
        while (pt_step_calls < target) {
            wdt_clear();
            if (estop_fast_requested) {
                rc = PB_TIME_ABORTED;
                break;
            }
            if ((unsigned long)(jiffies - j0) > guard) {
                rc = PB_TIME_TIMEOUT;
                break;
            }
        }
        n = pt_step_calls;
        dj = (u32)(jiffies - j0);
        sys_hi_timer_del(id);
    } else if (src == PT_JIFFY) {
        spin = (unsigned long)ms * 60000ul;
        while ((u32)(jiffies - j0) < target) {
            wdt_clear();
            if (estop_fast_requested) {
                rc = PB_TIME_ABORTED;
                break;
            }
            if (!--spin) {
                rc = PB_TIME_TIMEOUT;
                break;
            }
        }
        n = (u32)(jiffies - j0);
        dj = n;
    } else if (src == PT_TB) {
        u32 t0 = pb_tb_us();
        while ((n = (u32)(pb_tb_us() - t0)) < target) {
            wdt_clear();
            if (estop_fast_requested) {
                rc = PB_TIME_ABORTED;
                break;
            }
            if ((unsigned long)(jiffies - j0) > guard) {
                rc = PB_TIME_TIMEOUT;
                break;
            }
        }
        dj = (u32)(jiffies - j0);
    } else {
        unsigned step_us = src == PT_DELAY ? 1000u : 2u;
        for (n = 0; n < target; n++) {
            hal_delay_us(NULL, step_us);
            if (!(n & 0x3ffu)) { /* checked sparingly so the flag read does not skew the 2 us loop */
                wdt_clear();
                if (estop_fast_requested) {
                    rc = PB_TIME_ABORTED;
                    break;
                }
            }
        }
        dj = (u32)(jiffies - j0);
    }
    if (t3) pb_timebase_start(saved_wraps);
    out->n = n;
    out->want_us = want_us;
    out->dj = src == PT_JIFFY ? 0 : dj;
    out->ticks = ticks;
    out->div = t3 ? pt_t3[src].div : 0;
    out->isr = isr_n;
    return rc;
}
/* The stepper tick runs from the TIMER3 wrap ISR (pb_timebase_isr), one pb_step_tick per 1 ms while
 * core.step_active. Nothing to register or release any more; this only mirrors "a motion is running" into
 * core.step_timer_id, which the CONFIG-busy check and PDIAG still read. step_timer_callbacks counts ticks. */
static void sync_step_timer(void) {
    core.step_timer_id = core.step_active ? 1u : 0u;
}
static void command_task(void *priv) {
    u8 b[64];
    unsigned n, ticks = 0;
    const pb_hal h = {
        .set_mode = hal_mode,
        .write_port = hal_write,
        .write_port_fast = hal_write_fast,
        .read_port = hal_read,
        .adc_read = hal_adc,
        .millis = hal_ms,
        .tx_write = hal_tx,
        .delay_us = hal_delay_us,
        .heater_deadman_arm = hal_heater_deadman_arm,
        .heater_deadman_disarm = hal_heater_deadman_disarm,
        .time_test = hal_time_test,
        .tx_direct = hal_tx_direct,
        .micros = hal_micros,
#if PB_HEATER_TRACE
        .heater_trace = hal_heater_trace,
#endif
    };
    (void)priv;
    pb_timebase_init();
    pb_init(&core, &h);
    core.step_timer_id = 0;
    if (estop_fast_requested) core.emergency_abort_requested = 1;
    for (;;) {
        os_sem_pend(&cmd_sem, 1);
        if (detached) {
            local_irq_disable();
            detached = 0;
            usb_rx_rd = usb_rx_wr;
            rx_overflow = 0;
            local_irq_enable();
            core.llen = 0;
            core.discard = 0;
            pb_tbp_cancel_work(&core, 4u);
            pb_safe_state(&core);
            core.tbp_active = 0;
            pb_led_idle(&core, pb_now(&core));
            /* Keep the local power controls alive across USB detach. */
            { int k = pb_pin("PB1"); if (k >= 0) { (void)hal_mode(0,pb_defs[k].port,pb_defs[k].bit,PB_UP,1); core.pins[k].mode=PB_UP; } }
            { int k = pb_pin("PA1"); unsigned v=core.power_ready?1u:0u; if (k >= 0) { (void)hal_mode(0,pb_defs[k].port,pb_defs[k].bit,v?PB_HI:PB_LO,v); core.pins[k].mode=(uint8_t)(v?PB_HI:PB_LO); core.pins[k].level=(uint8_t)v; } }
            { int k = pb_pin("PC7"); if (k >= 0) { (void)hal_mode(0,pb_defs[k].port,pb_defs[k].bit,PB_LO,0); core.pins[k].mode=PB_LO; core.pins[k].level=0; } }
            { int k = pb_pin("PB11"); if (k >= 0) { (void)hal_mode(0,pb_defs[k].port,pb_defs[k].bit,PB_LO,0); core.pins[k].mode=PB_LO; core.pins[k].level=0; } }
        }
        if (rx_overflow) {
            local_irq_disable();
            rx_overflow = 0;
            usb_rx_rd = usb_rx_wr;
            fast_line_len = 0;
            fast_line_discard = 1;
            hello_fast_clear_requested = 0;
            local_irq_enable();
            core.llen = 0;
            core.discard = 1; /* discard through the next newline before parsing another frame */
            force_estop_after_rx_overflow();
        }
        while ((n = rx_ring_pop(b, sizeof b)) != 0) {
            pb_rx(&core, b, n);
            if (hello_fast_clear_requested && !core.tbp_stopped && !core.emergency_abort_requested) {
                estop_fast_requested = 0;
                hello_fast_clear_requested = 0;
            }
        }
        pb_poll(&core);
        sync_step_timer();
        hl_flush();
        if (++ticks >= 10) {
            ticks = 0;
            cdc_set_wakeup_handler(pb_wakeup);
        }
    }
}
static int idle_event_handler(struct application *app, struct sys_event *event) {
    (void)app;
    if (event->type == SYS_DEVICE_EVENT && event->arg == (void *)DEVICE_EVENT_FROM_OTG &&
        event->u.dev.event == DEVICE_EVENT_OUT) {
        detached = 1;
        request_fast_abort(0);
        os_sem_post(&rx_sem);
        os_sem_post(&cmd_sem);
    }
    return 0;
}
static int idle_state_machine(struct application *app, enum app_state state, struct intent *it) {
    (void)app;
    switch (state) {
    case APP_STA_START:
        if (it && it->action == ACTION_IDLE_MAIN) {
            clk_set("sys", BT_NORMAL_HZ);
            os_sem_create(&rx_sem, 0);
            os_sem_create(&cmd_sem, 0);
            os_sem_create(&tx_sem, 0);
            usb_start();
            cdc_set_wakeup_handler(pb_wakeup);
            os_task_create(command_task, 0, 2, 768, 0, "pb_cmd");
            os_task_create(rx_task, 0, 3, 768, 0, "pb_rx");
            os_task_create(tx_task, 0, 1, 512, 0, "pb_tx");
        }
        break;
    case APP_STA_DESTROY:
        detached = 1;
        request_fast_abort(0);
        os_sem_post(&rx_sem);
        os_sem_post(&cmd_sem);
        break;
    default:
        break;
    }
    return 0;
}
static const struct application_operation app_idle_ops = {.state_machine = idle_state_machine,
                                                          .event_handler = idle_event_handler};
REGISTER_APPLICATION(app_app_idle) = {
    .name = "idle", .action = ACTION_IDLE_MAIN, .ops = &app_idle_ops, .state = APP_STA_DESTROY};
/* Suppress all SDK logging: never leak text to CDC/UART. */
void log_print(int level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}
int printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}
int puts(const char *s) {
    (void)s;
    return 0;
}
int putchar(int c) {
    (void)c;
    return 0;
}
void put_buf(const u8 *buf, int len) {
    (void)buf;
    (void)len;
}
