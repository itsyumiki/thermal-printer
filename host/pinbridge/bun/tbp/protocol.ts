// TBP v1 constants (docs/thermal-protocol-spec.md). Pure data: no I/O, no imports.

/** Host -> printer opcodes (spec section 7). */
export const OP = {
  HELLO: 0x01,
  STAT: 0x02,
  FLUSH: 0x03,
  CONFIG: 0x04,
  ROW: 0x10,
  MOVE: 0x11,
  DWELL: 0x12,
  COILS: 0x20,
  /** Diagnostic (spec section 7.10): blocking timer self-test. Needs an idle printer. */
  TIMETEST: 0xf0,
  ESTOP: 0xff,
} as const;

/** Printer -> host response kinds (spec section 5). */
export const KIND = {
  ACK: 0x81,
  STATUS: 0x82,
  ERR: 0x83,
  EVENT: 0x84,
} as const;

/** First byte of a STATUS body (spec section 5.2). */
export const STATE = {
  QUEUED: 1,
  RUNNING: 2,
  DONE: 3,
  FAILED: 4,
  CANCELLED: 5,
  PROGRESS: 6,
} as const;

/** ERR and FAILED codes (spec section 10). */
export const ERR_CODE = {
  CRC: 0x01,
  FORMAT: 0x02,
  BADOP: 0x03,
  LEN: 0x04,
  RANGE: 0x05,
  SEQ: 0x06,
  QFULL: 0x07,
  STOPPED: 0x08,
  BUSY: 0x09,
  NOSESSION: 0x0a,
  VERSION: 0x0b,
  NOPAPER: 0x10,
  LOWBAT: 0x11,
  OVERTEMP: 0x12,
  SENSOR: 0x13,
  INTERNAL: 0x1f,
} as const;

/** CANCELLED reasons (spec section 5.2). */
export const CANCEL_REASON = {
  FLUSH: 1,
  ESTOP: 2,
  FAIL_STOP: 3,
  LINK_TIMEOUT: 4,
  NEW_SESSION: 5,
} as const;

/** EVENT types, always sent with id 0 (spec section 5.4). */
export const EVENT = {
  HEARTBEAT: 1,
  PAPER: 2,
  HOT: 3,
  RESET: 4,
  STOPPED: 5,
} as const;

/** Printer states reported by STAT and HEARTBEAT (spec section 6.4). */
export const PRINTER_STATE = {
  IDLE: 1,
  BUSY: 2,
  STOPPED: 3,
  WAIT_HOT: 4,
  NO_SESSION: 5,
} as const;

/** CONFIG keys (spec section 7.4). */
export const CONFIG_KEY = {
  HB_INTERVAL_MS: 1,
  IDLE_HOLD_MS: 2,
  LINK_TIMEOUT_MS: 3,
} as const;

/** TIMETEST sources (spec section 7.10). T3Dn = heater timer (TIMER3) at divider n; the order is the wire value. */
export const TIME_SRC = {
  T3D4: 0,
  T3D16: 1,
  T3D64: 2,
  T3D256: 3,
  STEP: 4,
  JIFFY: 5,
  DELAY: 6,
  DELAY2: 7,
  /** Sprint 2: the free-running microsecond timebase (TIMER3 /4) against the host clock. n counts microseconds. */
  TIMEBASE: 8,
} as const;

/** TIMETEST `STATUS FAILED` args, all with code `1F` INTERNAL (spec section 7.10). */
export const TIME_FAIL = {
  TIMEOUT: 1,
  ABORTED: 2,
  SETUP: 3,
} as const;

/** TIMETEST `ms` range accepted by the firmware (spec section 7.10). */
export const TIME_MS_MIN = 100;
export const TIME_MS_MAX = 3000;

/** Bits of the ROW DONE flags byte (spec section 7.5). */
export const ROW_FLAG = {
  COLD_START: 0x01,
  STRETCHED: 0x02,
  LATE: 0x04,
  DUTY_WAIT: 0x08,
  HOT_WAIT: 0x10,
  NO_HEAT: 0x20,
} as const;

/** Bits of the STAT flags byte (spec section 7.3). */
export const STAT_FLAG = {
  TEMP_GATE: 0x01,
  DUTY_GATE: 0x02,
  COILS: 0x04,
  HEATER: 0x08,
} as const;

/** Bits of the INFO caps byte (spec section 7.1). */
export const CAPS = {
  TEMP: 0x01,
  BATT: 0x02,
  BATT_MV: 0x04,
  PAPER: 0x08,
} as const;

/** Name of a numeric value in one of the tables above, or `undefined`. */
export function nameOf(
  table: Record<string, number>,
  value: number,
): string | undefined {
  for (const [name, v] of Object.entries(table)) if (v === value) return name;
  return undefined;
}

/** Names of the set bits of `flags` in a bit table, in table order. */
export function flagNames(
  table: Record<string, number>,
  flags: number,
): string[] {
  return Object.entries(table)
    .filter(([, bit]) => (flags & bit) !== 0)
    .map(([name]) => name);
}
