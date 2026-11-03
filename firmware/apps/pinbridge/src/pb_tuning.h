/*
 * Owner-editable printer tuning defaults.
 *
 * Provenance tags used below:
 *   STOCK    - taken from the original firmware (see thermal-printer/re/docs).
 *   OWNER    - decision by the owner on 2026-11-02 ("enable heaters and everything");
 *              numeric choices were derived from STOCK evidence and are NOT bench-validated.
 *   PROVISIONAL - starting point from the earlier WebUI/demo scripts; retune on the mechanism.
 *
 * Every limit here is enforced in firmware (pb_core.h); host/UI limits are only convenience.
 * Heater use is gated on valid head-temperature, battery and paper readings, a duty budget
 * and a per-slice maximum. Direct PB11-high through MOD/SET stays blocked on purpose: it
 * bypasses every one of those gates.
 */
#ifndef PB_TUNING_H
#define PB_TUNING_H

/* Printhead shift/latch timing: prior scripts used 2 / 1 / 2 microseconds. */
#define PB_TUNE_HEAD_CLOCK_HALF_US 2u       /* PROVISIONAL */
#define PB_TUNE_HEAD_DATA_SETUP_US 1u       /* PROVISIONAL */
#define PB_TUNE_HEAD_LATCH_PULSE_US 2u      /* PROVISIONAL */
#define PB_TUNE_HEAD_TIMING_MAX_US 1000u

/* ---- Heater limits ------------------------------------------------------------------
 * Stock drives PB11 for 25..75 timer ticks of ~30 us per print event (0.72..2.22 ms,
 * quality bytes 0x31..0x35) with a hard 200-tick (6 ms) cutoff, four events per row.   */
#define PB_TUNE_HEATER_SLICE_MAX_US 2500u   /* OWNER: stock best quality 2.22 ms/event + margin */
#define PB_TUNE_HEATER_NOMINAL_PULSE_US 1500u /* PROVISIONAL; old demo default */
#define PB_TUNE_HEATER_REQUEST_MAX_US PB_TUNE_HEATER_SLICE_MAX_US /* legacy PHEAT/PEVENT input */
#define PB_TUNE_HEATER_BURN_MAX_US 10000u   /* OWNER: >= 4 events x 2.22 ms (stock row at best quality) */
#define PB_TUNE_HEATER_TIMER_TICK_US 1u     /* software-timed (calibrated delay loop); pre-sprint-3 value */
#define PB_TUNE_HEATER_GUARD_US 100u        /* OWNER: minimum heater-off gap between slices (pre-sprint-3 value) */
#define PB_TUNE_HEATER_DUTY_WINDOW_MS 200u  /* OWNER */
#define PB_TUNE_HEATER_DUTY_BUDGET_US 60000u /* OWNER: 30 % of the window; stock full-black is ~33-37 % */
#define PB_TUNE_HEAD_SHIFT_TIME_US 3000u    /* ESTIMATE (pre-sprint-3 value), not scope-measured; host planning only */

/* ---- Head thermistor (PC4, SDK ADC channel 10) ---------------------------------------
 * STOCK: 10-bit ADC, 56 kOhm pull-up, NTC to ground, 101-entry resistance table at flash
 * 0x01E148B0 (pb_thermistor_table.h); index i = (i - 20) degC; raw > 899 = open sensor.
 * Conversion is ratiometric, so no ADC reference voltage is needed.                    */
#define PB_TUNE_THERMISTOR_PULLUP_OHMS 56000u /* STOCK */
#define PB_TUNE_ADC_FULL_SCALE 1023u          /* STOCK */
#define PB_TUNE_THERMISTOR_OPEN_RAW 899u      /* STOCK: raw above this means open/unplugged sensor */
#define PB_TUNE_THERMISTOR_SHORT_RAW 20u      /* OWNER: raw at/below this (R < ~1.1 kOhm) = shorted sensor; stock would read it as 80 degC */
/* Beta-model cross-check only (30 kOhm @ 25 degC, B ~3950 fits the stock table within +-3 % R, 0..80 degC). */
#define PB_TUNE_THERMISTOR_R0_OHMS 30000u
#define PB_TUNE_THERMISTOR_BETA_K 3950u
#define PB_TUNE_THERMISTOR_T0_C 25
#define PB_TUNE_ADC_REFERENCE_MV 3300u        /* VDDIO nominal; used only by the beta cross-check helpers */
#define PB_TUNE_TEMP_MIN_VALID_C 0            /* the stock table clamps everything <= 0 degC to 0 */
#define PB_TUNE_TEMP_MAX_VALID_C 80           /* the stock table saturates at 80 degC */
/* Gate: stock inhibits at >= 75 degC after 10 s and resumes below 50 degC. Firmware has no
 * 10 s tolerance, so the start gate sits 5 degC lower. */
#define PB_TUNE_TEMP_CUTOFF_C 70              /* OWNER: do not start heating at/above this */
#define PB_TUNE_TEMP_ABORT_C 75               /* STOCK inhibit level: abort a row already heating */
#define PB_TUNE_TEMP_RESUME_C 50              /* STOCK resume level: leave HOT wait below this */
#define PB_TUNE_HOT_WAIT_MAX_MS 60000u        /* OWNER: a row waiting for the head to cool fails OVERTEMP after this */

/* ---- Battery (SDK built-in quarter-VBAT channel, HAL already scales it to mV) ------- */
#define PB_TUNE_BATTERY_MIN_MV 3400u          /* OWNER: refuse to heat below this; verify `ADC vbat` against a meter */

/* ---- Paper (PB3, digital, no pulls) -------------------------------------------------
 * Paper present when PB3 reads low (the previous working status poll used this polarity;
 * stock ADC6 < 901 agrees).                                                             */
#define PB_TUNE_PAPER_PRESENT_LEVEL 0u

/* ---- Motor ------------------------------------------------------------------------- */
#define PB_TUNE_STEPPER_MIN_PERIOD_US 1000u /* PROVISIONAL firmware floor */
#define PB_TUNE_STEPPER_DEFAULT_PERIOD_US 1000u /* OWNER 2026-11-03: non-printing moves default to the 1 ms floor (was the 6 ms matrix demo) */
#define PB_TUNE_STEPPER_DEFAULT_GAP_US 1000u
#define PB_TUNE_STEPPER_DEFAULT_DWELL_US 1000u
#define PB_TUNE_COIL_HOLD_MS 0u /* conservative default: release coils */
#define PB_TUNE_COIL_HOLD_MAX_MS 10000u /* OWNER: longest idle coil hold CONFIG may request */
#define PB_TUNE_BOOT_WIGGLE_STEPS 200u /* OWNER: half-steps forward, then back, at power-on; 0 disables */
#define PB_TUNE_BOOT_WIGGLE_PERIOD_US 1000u /* OWNER 2026-11-03: per half-step (was 6000); the stock wiggle timing is not recovered */

#endif /* PB_TUNING_H */
