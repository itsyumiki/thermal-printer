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
#if !TCFG_USB_SLAVE_CDC_ENABLE
#error "pinbridge requires USB CDC"
#endif
#define PB_USB 0
#define PB_RING 1024
static pb_core core;
static OS_SEM rx_sem, cmd_sem, tx_sem;
static volatile u8 detached, rx_overflow, estop_fast_requested, hello_fast_clear_requested, fast_estop_id;
static u8 ring[PB_RING], usb_rx_ring[PB_RING];
static volatile u16 wr, rd, usb_rx_wr, usb_rx_rd;
static char fast_line[521];
static unsigned fast_line_len;
static u8 fast_line_discard;
static JL_PORT_FLASH_TypeDef *ports[4] = {JL_PORTA, JL_PORTB, JL_PORTC, JL_PORTD};
static int hal_mode(void *ctx, unsigned port, unsigned bit, unsigned mode, unsigned level);
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
/* Busy-wait calibrated on hardware (2026-11-02 --profile): the earlier 12 NOP-loop iterations
 * per us ran 10.9x too long, i.e. one iteration takes ~0.91 us, so ~1.1 iterations per us.
 * Still a software loop (not a timer): resolution is ~1 us and ISRs lengthen it slightly.
 * Do not use it as a certified heater safety timer. */
static void hal_delay_us(void *ctx, unsigned us) {
    volatile unsigned n;
    unsigned loops = (us * 11u + 5u) / 10u;
    (void)ctx;
    for (n = 0; n < loops; n++) {
        __asm__ volatile("nop");
    }
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
static void pb_step_timer(void *priv) {
    (void)priv;
    if (estop_fast_requested) {
        core.step_active = 0;
        core.step_done_pending = 0;
    } else pb_step_tick(&core);
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
    };
    (void)priv;
    pb_init(&core, &h);
    if (estop_fast_requested) core.emergency_abort_requested = 1;
    sys_s_hi_timer_add(NULL, pb_step_timer, 1);
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
            core.led_mode = 0;
            core.led_phase = 0;
            core.led_change_ms = pb_now(&core);
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
            local_irq_enable();
            core.llen = 0;
            core.discard = 0;
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
