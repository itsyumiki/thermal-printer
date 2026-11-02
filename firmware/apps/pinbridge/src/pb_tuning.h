/*
 * Owner-editable printer tuning defaults.
 *
 * Values marked PROVISIONAL reproduce the useful starting points from the prior
 * WebUI/demo scripts. Retune on the real mechanism after firmware changes.
 * Sensor conversion constants left at zero are intentionally UNSET: fill them
 * from measured calibration before implementing/enabling sensor capabilities.
 *
 * IMPORTANT: these are tuning inputs, NOT an enable switch for the heater.
 * The PB_TBP_* safety capability/limit constants in pb_core.h remain zero until
 * the complete sensor + timer + interlock path has been reviewed and validated.
 */
#ifndef PB_TUNING_H
#define PB_TUNING_H

/* Printhead shift/latch timing: prior scripts used 2 / 1 / 2 microseconds. */
#define PB_TUNE_HEAD_CLOCK_HALF_US 2u       /* PROVISIONAL */
#define PB_TUNE_HEAD_DATA_SETUP_US 1u       /* PROVISIONAL */
#define PB_TUNE_HEAD_LATCH_PULSE_US 2u      /* PROVISIONAL */
#define PB_TUNE_HEAD_TIMING_MAX_US 1000u

/* Heater values from the previous demo/UI. Not validated safety limits. */
#define PB_TUNE_HEATER_NOMINAL_PULSE_US 1500u /* PROVISIONAL; old demo default */
#define PB_TUNE_HEATER_REQUEST_MAX_US 6000u   /* legacy input ceiling only */

/* Motion starting points from prior scripts. */
#define PB_TUNE_STEPPER_MIN_PERIOD_US 1000u /* PROVISIONAL firmware floor */
#define PB_TUNE_STEPPER_DEFAULT_PERIOD_US 6000u /* prior matrix demo: 6 ms */
#define PB_TUNE_STEPPER_DEFAULT_GAP_US 1000u
#define PB_TUNE_STEPPER_DEFAULT_DWELL_US 1000u
#define PB_TUNE_COIL_HOLD_MS 0u /* conservative default: release coils */

/* Sensor calibration inputs. Zero means NOT SET; no conversion may assume otherwise. */
#define PB_TUNE_THERMISTOR_PULLUP_OHMS 56000u /* value recorded in owner ledger */
#define PB_TUNE_THERMISTOR_R0_OHMS 0u
#define PB_TUNE_THERMISTOR_BETA_K 0u
#define PB_TUNE_THERMISTOR_T0_C 0
#define PB_TUNE_ADC_REFERENCE_MV 0u
#define PB_TUNE_BATTERY_DIVIDER_NUMERATOR 0u
#define PB_TUNE_BATTERY_DIVIDER_DENOMINATOR 0u
#define PB_TUNE_TEMP_MIN_VALID_C 0
#define PB_TUNE_TEMP_MAX_VALID_C 0
#define PB_TUNE_TEMP_CUTOFF_C 0
#define PB_TUNE_BATTERY_MIN_MV 0u

/* Unknown until a measured timer/duty budget is entered; heater gate stays shut. */
#define PB_TUNE_HEATER_TIMER_TICK_US 0u
#define PB_TUNE_HEATER_GUARD_US 0u
#define PB_TUNE_HEATER_DUTY_WINDOW_MS 0u
#define PB_TUNE_HEATER_DUTY_BUDGET_US 0u
#define PB_TUNE_HEAD_SHIFT_TIME_US 0u

#endif /* PB_TUNING_H */
