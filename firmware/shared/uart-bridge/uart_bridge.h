/*
 * uart_bridge.h -- shared core of the two USB-CDC <-> Bluetooth bridge firmwares
 * (ble-uart-serial and classic-spp-uart-serial).
 *
 * THIS FILE AND uart_bridge.c ARE BYTE-IDENTICAL IN BOTH VARIANTS. Edit them only in
 * firmware/shared/uart-bridge/ and run scripts/sync-uart-bridge.sh (see
 * scripts/check-shared-core.sh). Everything that differs between BLE and Classic SPP
 * lives in the transport glue (an overlay of an SDK file), which only uses the API below.
 *
 * What the core does (CDC = the USB virtual COM port, BT = the Bluetooth peer):
 *   CDC rx -> ring A -> chunks of the BT payload size -> bridge_bt_send() (glue)
 *   BT rx  -> bridge_bt_rx() (glue) -> ring B -> CDC tx
 *   SDK/Bluetooth logs (log_print, printf, puts, putchar, put_buf are overridden) ->
 *   log ring -> CDC tx, one whole line at a time, prefixed with "[BT] ".
 * See the README of each variant for behaviour, limits and what is untested.
 *
 * The compile-time switches below can be overridden with -D, but the build scripts of
 * the repo do not pass any, so change the default here (then run the sync script).
 */
#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include "typedef.h"

/* ---- switches ------------------------------------------------------------------ */

/* 1: SDK/Bluetooth logs and bridge events are sent to the CDC port, as "[BT] ..." lines
 *    between data chunks.  0: every log is discarded (the overrides stay, so nothing
 *    reaches the SDK UART either) and the CDC stream carries only bridged data. */
#ifndef BRIDGE_CDC_LOGS
#define BRIDGE_CDC_LOGS            1
#endif

/* Log levels as in the SDK: 0 verbose, 1 debug, 2 info, 3 warn, 4 error. Lower levels
 * passed to log_print() are discarded. printf/puts/putchar/put_buf count as level 2. */
#ifndef BRIDGE_LOG_MIN_LEVEL
#define BRIDGE_LOG_MIN_LEVEL       0
#endif

/* 1: read the CDC OUT endpoint with the SDK's cdc_read_data() instead of calling
 *    usb_g_bulk_read() directly. cdc_read_data() shares a mutex with the CDC write, so a
 *    stalled host that blocks a write can also block the read (see README). */
#ifndef BRIDGE_RX_VIA_SDK_READ
#define BRIDGE_RX_VIA_SDK_READ     0
#endif

/* Define the btstack debug-level variables (ble/l2cap/profile/rfcomm_debug_enable) so
 * the stack prints its internal traces. 0 leaves the library defaults. */
#ifndef BRIDGE_BT_STACK_DEBUG
#define BRIDGE_BT_STACK_DEBUG      1
#endif

/* ---- sizes (RAM is static, see README) ----------------------------------------- */
#ifndef BRIDGE_RING_A_SIZE
#define BRIDGE_RING_A_SIZE         2048    /* CDC -> BT */
#endif
#ifndef BRIDGE_RING_B_SIZE
#define BRIDGE_RING_B_SIZE         2048    /* BT -> CDC */
#endif
#ifndef BRIDGE_LOG_RING_SIZE
#define BRIDGE_LOG_RING_SIZE       3072    /* log records, [len][bytes] */
#endif
#ifndef BRIDGE_MAX_CHUNK
#define BRIDGE_MAX_CHUNK           512     /* largest BT payload the core will build */
#endif
#ifndef BRIDGE_MIN_CHUNK
#define BRIDGE_MIN_CHUNK           20
#endif
#ifndef BRIDGE_LOG_REC_MAX
#define BRIDGE_LOG_REC_MAX         200     /* longest log line incl. "[BT] " (<= 250) */
#endif
#ifndef BRIDGE_LOG_FMT_MAX
#define BRIDGE_LOG_FMT_MAX         160     /* format buffer, one for tasks, one for IRQs */
#endif
#ifndef BRIDGE_HEXDUMP_MAX
#define BRIDGE_HEXDUMP_MAX         64      /* put_buf() prints at most this many bytes */
#endif

/* ---- timing (OS tick = 10 ms, HZ 100) ------------------------------------------ */
#ifndef BRIDGE_IDLE_TICKS
#define BRIDGE_IDLE_TICKS          1       /* flush a partial chunk after >= 1 tick idle */
#endif
#ifndef BRIDGE_MAX_HOLD_TICKS
#define BRIDGE_MAX_HOLD_TICKS      5       /* ... or when its oldest byte is this old */
#endif
#ifndef BRIDGE_STATS_PERIOD_TICKS
#define BRIDGE_STATS_PERIOD_TICKS  500     /* stats line every 5 s, only if counters moved */
#endif

#define BRIDGE_LOG_PREFIX          "[BT] "

/* ---- result codes of bridge_bt_send() (implemented by the glue) ---------------- */
enum {
    BR_TX_OK = 0,        /* the BT stack took the whole chunk                           */
    BR_TX_BUSY,          /* transient: stack buffer full; the chunk stays queued, retry */
    BR_TX_TOO_BIG,       /* chunk larger than the transport accepts; core halves it     */
    BR_TX_NOT_READY,     /* no peer / notifications off: data is dropped and counted    */
    BR_TX_FAIL,          /* other error: this chunk is dropped and counted              */
};

/* ---- API used by the transport glue -------------------------------------------- */
void bridge_init(void);                       /* idempotent; call as early as possible   */
void bridge_peer_connected(u16 chunk);        /* peer usable; chunk = BT payload bytes   */
void bridge_peer_disconnected(void);          /* queued CDC->BT data is dropped, counted */
void bridge_set_chunk(u16 chunk);             /* e.g. after the BLE MTU exchange         */
void bridge_bt_rx(const u8 *data, u16 len);   /* BT -> CDC; drop + count if ring B full  */
void bridge_tx_ready(void);                   /* the BT stack can take data again        */
void bridge_log(const char *fmt, ...);        /* app event, shows up as "[BT] app: ..."  */

/* ---- implemented by the glue, called from the core's CDC->BT task --------------- */
int bridge_bt_send(const u8 *data, u16 len);  /* returns BR_TX_*                         */

#endif /* UART_BRIDGE_H */
