/*
 * hello-cdc -- application source (this is the file to edit).
 *
 * Derived from blink-pc7 / the SDK example apps/spp_and_le/examples/idle/app_idle.c;
 * the build copies this file over that path (see ../app.conf). Still the SDK's
 * "idle" application: no Bluetooth, just the event loop.
 *
 * What it does: the chip enumerates on USB as a CDC serial port (a virtual COM
 * port / /dev/ttyACM0 on the PC) and sends "hello world 1", "hello world 2", ...
 * once a second, forever. Baud rate / 8N1 settings on the PC side do not matter,
 * this is USB, not a real UART. Each line ends in "\r\n".
 *
 * The counter only advances when a line was actually delivered. The SDK refuses
 * writes until the PC has OPENED the port, so open your terminal at any time and
 * you will see "hello world 1" first.
 *
 * It also toggles PC7 (the pin blink-pc7 proved is an LED) as a heartbeat:
 *   slow (1 s)    : port is open, lines are being sent
 *   fast (250 ms) : waiting for the PC to open the port (or USB not connected)
 *
 * USB uses the chip's dedicated DP/DM pins, so there is no GPIO pin to configure.
 * Anything the PC types is echoed back and hexdumped by the SDK (task_pc.c).
 */
#include "system/app_core.h"
#include "system/includes.h"
#include "server/server_core.h"
#include "app_config.h"
#include "app_action.h"
#include "usb/device/cdc.h"

#define LOG_TAG_CONST       APP_IDLE
#define LOG_TAG             "[APP_IDLE]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"


#if CONFIG_APP_IDLE || CONFIG_APP_SPP_LE_TO_IDLE

#if CONFIG_APP_IDLE
#if TCFG_USER_EDR_ENABLE || TCFG_USER_BLE_ENABLE
//默认应用不需要打开配置,有需要自己添加
#error " confirm, need disable !!!!!!"
#endif
#endif

#if !TCFG_USB_SLAVE_CDC_ENABLE
#error "USB CDC is not enabled: app.conf must set TCFG_PC_ENABLE and USB_DEVICE_CLASS_CONFIG"
#endif

/* ---- settings you are expected to change ---------------------------------- */
#define HELLO_PERIOD_MS      1000          /* time between lines */
#define WAIT_PERIOD_MS       250           /* retry / fast-blink period until the port is open */
#define ALIVE_PIN            IO_PORTC_07   /* heartbeat LED */
/* ---------------------------------------------------------------------------- */

#define HELLO_USB_ID         0
#define HELLO_TASK_NAME      "hello_cdc"

static volatile u8 hello_run;
extern void app_switch(const char *name, int action);

static void app_key_event_handler(struct sys_event *event)
{
    /* u16 cpi = 0; */
    u8 event_type = 0;
    u8 key_value = 0;
    if (event->arg == (void *)DEVICE_EVENT_FROM_KEY) {
        event_type = event->u.key.event;
        key_value = event->u.key.value;
        log_info("idle_key_evnet: %d,%d\n", event_type, key_value);
#if CONFIG_APP_SPP_LE_TO_IDLE
        if (event_type == KEY_EVENT_CLICK && key_value == TCFG_ADKEY_VALUE1) {
            log_info(">>>hold key1:change case to spp_le\n");
            //change case to spp_le test
            app_switch("spp_le", ACTION_SPPLE_MAIN);
        }
#endif
    }
}

/* Write "hello world <n>\r\n" into buf, return its length. No libc printf needed. */
static u32 make_line(char *buf, u32 n)
{
    static const char prefix[] = "hello world ";
    char digits[10];
    u32 len = 0, nd = 0, i;

    for (i = 0; i < sizeof(prefix) - 1; i++) {
        buf[len++] = prefix[i];
    }
    do {                                  /* decimal, least significant digit first */
        digits[nd++] = '0' + (n % 10);
        n /= 10;
    } while (n);
    while (nd) {
        buf[len++] = digits[--nd];
    }
    buf[len++] = '\r';
    buf[len++] = '\n';
    return len;
}

/*
 * Runs in its own OS task: cdc_write_data() takes a mutex and waits for the USB
 * transfer, so it must not be called from a sys_timer callback or an interrupt.
 */
static void hello_task(void *priv)
{
    char line[32];
    u32 n = 1, len, period_ms;
    u8 level = 0;

    while (hello_run) {
        len = make_line(line, n);
        if (cdc_write_data(HELLO_USB_ID, (u8 *)line, len) == len) {
            n++;                          /* delivered: next time send the next number */
            period_ms = HELLO_PERIOD_MS;
        } else {
            period_ms = WAIT_PERIOD_MS;   /* port not open yet (or USB unplugged): retry */
        }
        level ^= 1;
        gpio_write(ALIVE_PIN, level);
        os_time_dly(period_ms / 10);      /* 1 tick = 10 ms */
    }

    usb_stop();
    os_task_del(OS_TASK_SELF);
}

static void idle_app_start()
{
    log_info("=======================================");
    log_info("---------hello-cdc---------");
    log_info("=======================================");

    log_info("app_file: %s", __FILE__);

    clk_set("sys", BT_NORMAL_HZ);

    gpio_set_die(ALIVE_PIN, 1);
    gpio_set_pull_up(ALIVE_PIN, 0);
    gpio_set_pull_down(ALIVE_PIN, 0);
    gpio_direction_output(ALIVE_PIN, 0);

    usb_start();                          /* enumerate as a CDC serial device */

    hello_run = 1;
    if (os_task_create(hello_task, NULL, 1, 512, 0, HELLO_TASK_NAME) != 0) {
        log_error("hello task create failed\n");
    }
}

static int idle_event_handler(struct application *app, struct sys_event *event)
{
    switch (event->type) {
    case SYS_KEY_EVENT:
        app_key_event_handler(event);
        return 0;
    case SYS_BT_EVENT:
        return 0;
    case SYS_DEVICE_EVENT:
        return 0;
    default:
        return false;
    }
}


static int idle_state_machine(struct application *app, enum app_state state,
                              struct intent *it)
{
    switch (state) {
    case APP_STA_CREATE:
        break;
    case APP_STA_START:
        if (!it) {
            break;
        }
        switch (it->action) {
        case ACTION_IDLE_MAIN:
            log_info("ACTION_IDLE_MAIN\n");
            /* os_taskq_flush(); */
            idle_app_start();
            break;
        }
        break;
    case APP_STA_PAUSE:
        break;
    case APP_STA_RESUME:
        break;
    case APP_STA_STOP:
        break;
    case APP_STA_DESTROY:
        hello_run = 0;                    /* the task stops USB and deletes itself */
        break;
    }

    return 0;
}

static const struct application_operation app_idle_ops = {
    .state_machine  = idle_state_machine,
    .event_handler 	= idle_event_handler,
};

REGISTER_APPLICATION(app_app_idle) = {
    .name 	= "idle",
    .action	= ACTION_IDLE_MAIN,
    .ops 	= &app_idle_ops,
    .state  = APP_STA_DESTROY,
};

#endif
