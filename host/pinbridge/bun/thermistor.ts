import { THERMISTOR_TABLE_OHMS } from "./thermistor-table.ts";

const ADC_FULL_SCALE = 1023;
const PULLUP_OHMS = 56000;
const OPEN_RAW = 899;
const SHORT_RAW = 20;

/** Match pb_sensor_head_temp_c: stock-table lookup, rounded upward to a whole degree C. */
export function headTempCFromRaw(raw: number): number | null {
  if (!Number.isInteger(raw) || raw > OPEN_RAW || raw <= SHORT_RAW) return null;
  const resistanceOhms = Math.floor(
    (raw * PULLUP_OHMS) / (ADC_FULL_SCALE - raw),
  );
  let index = THERMISTOR_TABLE_OHMS.findIndex(
    (resistance) => resistance <= resistanceOhms,
  );
  if (index < 0) index = THERMISTOR_TABLE_OHMS.length - 1;
  return Math.max(0, index - 20);
}
