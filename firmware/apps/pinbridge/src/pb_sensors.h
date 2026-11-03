/* Calibration-gated sensor math shared by firmware and portable C tests. */
#ifndef PB_SENSORS_H
#define PB_SENSORS_H
#include <stdint.h>
#include "pb_tuning.h"
#include "pb_thermistor_table.h"

/* True only when every input needed for the temperature conversion and gate is set and sane. */
static inline int pb_sensor_temperature_calibrated(void) {
    return PB_TUNE_THERMISTOR_PULLUP_OHMS > 0u && PB_TUNE_ADC_FULL_SCALE > 1u &&
           PB_TUNE_THERMISTOR_SHORT_RAW < PB_TUNE_THERMISTOR_OPEN_RAW &&
           PB_TUNE_THERMISTOR_OPEN_RAW < PB_TUNE_ADC_FULL_SCALE &&
           PB_TUNE_TEMP_MIN_VALID_C < PB_TUNE_TEMP_MAX_VALID_C &&
           PB_TUNE_TEMP_RESUME_C < PB_TUNE_TEMP_CUTOFF_C &&
           PB_TUNE_TEMP_CUTOFF_C <= PB_TUNE_TEMP_ABORT_C &&
           PB_TUNE_TEMP_CUTOFF_C > PB_TUNE_TEMP_MIN_VALID_C &&
           PB_TUNE_TEMP_ABORT_C <= PB_TUNE_TEMP_MAX_VALID_C;
}

/* Battery millivolts come from the SDK's calibrated quarter-VBAT channel (the HAL scales by
 * four); no board divider is involved, so only the cutoff has to be set. */
static inline int pb_sensor_battery_calibrated(void) {
    return PB_TUNE_BATTERY_MIN_MV > 0u;
}

/* Stock head-temperature conversion, bit-exact with app_2ms_adc_and_print_tick (0x01E0045A):
 *   raw > 899 -> sentinel (open sensor); else R = raw * 56000 / (1023 - raw);
 *   idx = first i with table[i] <= R (101 if none, clamped to 100); temp = idx > 20 ? idx - 20 : 0.
 * Additionally a raw count at or below PB_TUNE_THERMISTOR_SHORT_RAW is rejected as a shorted
 * sensor (stock would report it as 80 degC). Returns 1 and sets *temp_c on success; on failure
 * the output is left untouched. The reported value is ceil(true temperature): biased high by up
 * to 1 degC, the safe direction for an over-temperature guard. */
static inline int pb_sensor_head_temp_c(unsigned raw, int *temp_c) {
    uint32_t r, idx;
    if (!temp_c || raw > PB_TUNE_THERMISTOR_OPEN_RAW || raw <= PB_TUNE_THERMISTOR_SHORT_RAW)
        return 0;
    r = ((uint32_t)raw * PB_TUNE_THERMISTOR_PULLUP_OHMS) / (PB_TUNE_ADC_FULL_SCALE - raw);
    for (idx = 0; idx < PB_THERM_TABLE_N; idx++)
        if (pb_therm_table[idx] <= r)
            break;
    if (idx > PB_THERM_TABLE_N - 1u)
        idx = PB_THERM_TABLE_N - 1u;
    *temp_c = idx > 20u ? (int)idx - 20 : 0;
    return 1;
}

/* Largest raw count whose reported temperature is still >= temp_c (hotter readings have lower
 * raw counts because the NTC is the low side of the divider). Returns 0 if none. Used for the
 * INFO temp_limit_raw field: the head is too hot when raw <= this value. */
static inline unsigned pb_sensor_hot_raw_limit(int temp_c) {
    unsigned lo = PB_TUNE_THERMISTOR_SHORT_RAW + 1u, hi = PB_TUNE_THERMISTOR_OPEN_RAW;
    unsigned best = 0u;
    int t;
    while (lo <= hi) {
        unsigned mid = (lo + hi) / 2u;
        if (pb_sensor_head_temp_c(mid, &t) && t >= temp_c) {
            best = mid;
            lo = mid + 1u;
        } else {
            if (mid == 0u) break;
            hi = mid - 1u;
        }
    }
    return best;
}

/* Convert an ADC pin voltage into NTC resistance for a pull-up-to-Vref divider.
 * Returns 0 for invalid electrical inputs; output is untouched on failure.
 * This deliberately does not claim a temperature or a healthy sensor state. */
static inline int pb_sensor_thermistor_ohms(unsigned pin_mv, unsigned reference_mv,
                                    unsigned pullup_ohms, unsigned *resistance_ohms) {
    uint64_t numerator;
    unsigned denominator;
    uint64_t result;
    if (!resistance_ohms || !pin_mv || !reference_mv || !pullup_ohms || pin_mv >= reference_mv)
        return 0;
    denominator = reference_mv - pin_mv;
    numerator = (uint64_t)pullup_ohms * pin_mv;
    result = numerator / denominator;
    if (!result || result > 0xffffffffu)
        return 0;
    *resistance_ohms = (unsigned)result;
    return 1;
}

/* Fixed-point natural log for positive Q16 values. Returns ln(value) in Q16. */
static inline int64_t pb_sensor_ln_q16(uint64_t value_q16) {
    const int64_t one = 65536;
    const int64_t ln2_q16 = 45426;
    int exponent = 0;
    int64_t m, z, z2, term, sum;
    if (!value_q16) return INT64_MIN;
    while (value_q16 < (uint64_t)one) { value_q16 <<= 1; exponent--; }
    while (value_q16 >= (uint64_t)(2 * one)) { value_q16 >>= 1; exponent++; }
    m = (int64_t)value_q16;
    z = ((m - one) * one) / (m + one);
    z2 = (z * z) / one;
    term = z;
    sum = term;
    term = (term * z2) / one; sum += term / 3;
    term = (term * z2) / one; sum += term / 5;
    term = (term * z2) / one; sum += term / 7;
    term = (term * z2) / one; sum += term / 9;
    term = (term * z2) / one; sum += term / 11;
    return 2 * sum + (int64_t)exponent * ln2_q16;
}

/* Beta-equation NTC conversion. Output is milli-degrees C; reject implausible
 * denominator/results and leave output untouched on failure. */
static inline int pb_sensor_temperature_mC(unsigned resistance_ohms, unsigned r0_ohms,
                                            unsigned beta_k, int t0_c,
                                            int *temperature_mC) {
    const int64_t q16 = 65536;
    int64_t ratio_q16, ln_ratio, t0_mK, inverse_q16, kelvin_mK, celsius_mC;
    if (!temperature_mC || !resistance_ohms || !r0_ohms || !beta_k || t0_c <= -273)
        return 0;
    ratio_q16 = ((uint64_t)resistance_ohms * (uint64_t)q16) / r0_ohms;
    if (!ratio_q16) return 0;
    ln_ratio = pb_sensor_ln_q16((uint64_t)ratio_q16);
    t0_mK = ((int64_t)t0_c + 273) * 1000 + 150;
    /* 1/T = 1/T0 + ln(R/R0)/Beta; keep reciprocal in Q16 per kelvin. */
    inverse_q16 = (q16 * 1000000 / t0_mK) + (ln_ratio * 1000 / (int64_t)beta_k);
    if (inverse_q16 <= 0) return 0;
    kelvin_mK = q16 * 1000000 / inverse_q16;
    celsius_mC = kelvin_mK - 273150;
    if (celsius_mC < -100000 || celsius_mC > 500000) return 0;
    *temperature_mC = (int)celsius_mC;
    return 1;
}

/* Convert the SDK-reported divider pin voltage into rail mV. */
static inline int pb_sensor_battery_mv(unsigned pin_mv, unsigned numerator, unsigned denominator,
                                unsigned *battery_mv) {
    uint64_t scaled;
    if (!battery_mv || !pin_mv || !numerator || !denominator)
        return 0;
    scaled = (uint64_t)pin_mv * numerator;
    scaled /= denominator;
    if (!scaled || scaled > 0xffffffffu)
        return 0;
    *battery_mv = (unsigned)scaled;
    return 1;
}
#endif
