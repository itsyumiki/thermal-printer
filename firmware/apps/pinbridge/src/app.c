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
#define PB_HEATER_DEADMAN_MARGIN_US 100u /* setup allowance; wrap ISR adds up to another 10 ms */
static pb_core core;
static u16 step_timer_id;
static volatile u8 heater_timer_armed, timebase_ready, timebase_stalled;
static volatile u32 heater_deadman_at;
static volatile u32 tb_wraps, tb_isr_con; /* timebase wrap count and the CON value the wrap ISR writes back */
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
    (void)ctx;
    local_irq_disable();
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
    local_irq_enable();
    return 0;
}
static void hal_write(void *ctx, unsigned port, uint16_t mask, uint16_t vals) {
    (void)ctx;
    local_irq_disable();
    ports[port]->OUT = (ports[port]->OUT & ~mask) | (vals & mask);
    local_irq_enable();
}
static void hal_write_fast(void *ctx, unsigned port, uint16_t mask, uint16_t vals) {
    (void)ctx;
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
 * TIMER3 is a free-running microsecond timebase: 24 MHz oscillator / 4 = 6 ticks per us, PRD 60000 = one wrap
 * (and one ISR) every 10 ms exactly. Why /4 and 10 ms: the finest resolution (1/6 us) that still fits a whole
 * number of microseconds into PRD, and a wrap rate low enough that the ISR costs about 0.1 % of the CPU, while
 * 10 ms is also the period Sprint 3 wants for the heater deadman. /16 would need a 40 ms wrap for the same
 * PRD and coarser reads; /64 and /256 only hurt resolution. The prescale index comes from the SDK irflt.c
 * table (1 = /4); Sprint 1 measured the oscillator at 23.97-24.00 MHz for every prescaler.
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
    tb_wraps++; /* before the pending flag is cleared: pb_tb_us() relies on this order */
    core.heater_isr_count++; /* every TIMER3 ISR, kept for the TIMETEST TIMER3 rows */
    if (pb_heater_deadman_expire(&heater_timer_armed,
            (tb_wraps * PB_TB_PERIOD_US) + ((PB_TB_TIMER->CNT & 0xffffu) / PB_TB_TICKS_PER_US),
            heater_deadman_at, &core.heater_deadman_trips)) {
        /* TIMER3 wraps every 10 ms: the main loop normally ends the pulse at its exact
         * deadline plus the 100 us setup allowance; this independent cutoff is bounded by one
         * additional wrap period after that margin. */
        ports[1]->OUT &= ~(1u << 11);
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
    local_irq_enable();
    return 0;
}
static void hal_heater_deadman_disarm(void *ctx) {
    (void)ctx;
    local_irq_disable();
    ports[1]->OUT &= ~(1u << 11);
    heater_timer_armed = 0;
    local_irq_enable();
}
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
    if (src == PT_STEP && step_timer_id) return PB_TIME_SETUP;
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
static void pb_step_timer(void *priv) {
    (void)priv;
    core.step_timer_callbacks++;
    if (estop_fast_requested) {
        core.step_active = 0;
        core.step_done_pending = 0;
    } else pb_step_tick(&core);
}
/* The SDK's low-power-safe sys_s_hi_timer may stretch its period during sleep.
 * Use the awake 1 ms timer only while motion/boot wiggle is active, then release it. */
static void sync_step_timer(void) {
    if (core.step_active) {
        if (!step_timer_id) {
            step_timer_id = sys_hi_timer_add(NULL, pb_step_timer, 1);
            core.step_timer_id = step_timer_id;
            if (!step_timer_id) {
                core.step_timer_reg_failures++;
                /* Fail safe rather than leave an accepted motion waiting on a missing timer. */
                pb_tbp_cancel_work(&core, 3u);
                pb_safe_state(&core);
            }
        }
    } else if (step_timer_id) {
        sys_hi_timer_del(step_timer_id);
        step_timer_id = 0;
        core.step_timer_id = 0;
    }
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
    };
    (void)priv;
    pb_timebase_init();
    pb_init(&core, &h);
    step_timer_id = 0;
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
