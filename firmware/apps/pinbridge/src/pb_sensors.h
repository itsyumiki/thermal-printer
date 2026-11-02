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
