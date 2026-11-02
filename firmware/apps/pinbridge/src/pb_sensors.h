/* Calibration-gated sensor math shared by firmware and portable C tests. */
#ifndef PB_SENSORS_H
#define PB_SENSORS_H
#include <stdint.h>
#include "pb_tuning.h"

/* True only when every input needed for a future temperature conversion is set. */
static inline int pb_sensor_temperature_calibrated(void) {
    return PB_TUNE_THERMISTOR_PULLUP_OHMS > 0u && PB_TUNE_THERMISTOR_R0_OHMS > 0u &&
           PB_TUNE_THERMISTOR_BETA_K > 0u && PB_TUNE_ADC_REFERENCE_MV > 0u &&
           PB_TUNE_TEMP_MIN_VALID_C < PB_TUNE_TEMP_MAX_VALID_C &&
           PB_TUNE_TEMP_CUTOFF_C > PB_TUNE_TEMP_MIN_VALID_C &&
           PB_TUNE_TEMP_CUTOFF_C <= PB_TUNE_TEMP_MAX_VALID_C;
}

static inline int pb_sensor_battery_calibrated(void) {
    return PB_TUNE_ADC_REFERENCE_MV > 0u && PB_TUNE_BATTERY_DIVIDER_NUMERATOR > 0u &&
           PB_TUNE_BATTERY_DIVIDER_DENOMINATOR > 0u && PB_TUNE_BATTERY_MIN_MV > 0u;
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
