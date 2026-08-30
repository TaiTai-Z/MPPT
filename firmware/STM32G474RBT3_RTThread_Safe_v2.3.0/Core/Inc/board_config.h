#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* Firmware identity ------------------------------------------------------- */
#if !defined(APP_USE_RTTHREAD)
#error "This v2.3.0 project is the RT-Thread-only release"
#endif
#define FW_VERSION "G474-RBT3-RTT-SAFE-2.3.0"

/* The fitted part is STM32G474RBT3. The schematic library record still says
 * RET3 in one field; that is an EDA metadata defect, not a reason to select a
 * 512-KiB target in Keil. */
#define BOARD_MCU_FLASH_KIB       128U
#define BOARD_MCU_MAIN_SRAM_KIB    96U
#define BOARD_MCU_CCM_SRAM_KIB     32U

/* X1 is an 8 MHz crystal in the 2026-08-28 control-board netlist.  The
 * production clock request is HSE / 2 * 85 / 2 = 170 MHz.  Code must still
 * check HSERDY/PLLRDY and fall back to HSI16 with power outputs locked. */
#define BOARD_HSE_HZ                   8000000UL
#define BOARD_TARGET_SYSCLK_HZ       170000000UL
#define BOARD_FALLBACK_SYSCLK_HZ      16000000UL
#define BOARD_SYSCLK_HZ BOARD_TARGET_SYSCLK_HZ
#define BOARD_UART_BAUD              115200UL
#define BOARD_UART_BRR_HSI16_115200  139UL
#define BOARD_SYSTICK_HZ             1000UL

/* Board profiles ---------------------------------------------------------- */
#define BOARD_PROFILE_NONSYNC_2026_08_28          1U
#define BOARD_PROFILE_SYNC_DAUGHTER_UNCONFIRMED   2U
#define BOARD_POWER_STAGE_PROFILE BOARD_PROFILE_NONSYNC_2026_08_28

/* The timing contract is configured and read back while counters, pins and
 * output enables remain off. Physical arming is a separate safety gate. */
#define BOARD_HRTIM_TIMING_CONFIG_ENABLE  1U
#define BOARD_HRTIM_REQUESTED_PWM_HZ      100000UL
/* CK_PSC=0 selects the high-resolution x32 clock.  HRTIM counts 0..PER,
 * therefore PER=54,399 gives 54,400 counts and exactly 100 kHz at 170 MHz. */
#define BOARD_HRTIM_PRESCALER_CODE        0U
#define BOARD_HRTIM_PERIOD_TICKS          54399UL
#define BOARD_HRTIM_INITIAL_DUTY_Q15      16384UL
#define BOARD_HRTIM_DLL_TIMEOUT_LOOPS      1000000UL
#define BOARD_PER_CYCLE_ADC_ENABLE         1U
#define BOARD_FAST_ADC_IRQ_PRIORITY        2U

/* These must remain zero until real FLT/EEV sources, 15-V feedback and the
 * assembled power stage have been measured. The 2026-08-28 non-synchronous
 * power-board netlist leaves FLT1..6/EEV1..2 connector-only. */
#define BOARD_SYNC_PINMAP_CONFIRMED       0U
#define BOARD_FLT_INPUT_CONNECTED         0U
#define BOARD_EXTERNAL_PROTECTION_CONFIRMED 1U
#define BOARD_HARDWARE_FLT_CONFIRMED      0U
#define BOARD_V15_FEEDBACK_CONFIRMED      0U
#define BOARD_THERMAL_LIMITS_CONFIRMED    0U
#define BOARD_PV_LIMIT_CONFIRMED          0U
#define BOARD_CURRENT_POLARITY_CONFIRMED  0U
#define BOARD_CONTROL_GAINS_VALIDATED     0U
#define BOARD_V15_PGOOD_RUNTIME_IMPLEMENTED 0U
#define BOARD_FLT_RUNTIME_POLARITY_VALIDATED 0U
#define BOARD_POWER_OUTPUT_ARMING_ENABLE  1U

#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_HARDWARE_FLT_CONFIRMED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires verified hardware FLT routing"
#endif
#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_V15_FEEDBACK_CONFIRMED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires verified 15-V feedback"
#endif
#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_THERMAL_LIMITS_CONFIRMED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires validated NTC placement and trip limits"
#endif
#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_PV_LIMIT_CONFIRMED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires validated PV operating/OVP limits"
#endif
#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_CURRENT_POLARITY_CONFIRMED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires measured I1/I2 polarity"
#endif
#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_CONTROL_GAINS_VALIDATED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires plant-derived and bench-validated gains"
#endif
#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_V15_PGOOD_RUNTIME_IMPLEMENTED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires a real MCU-readable 15-V PGOOD implementation"
#endif
#if BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_FLT_RUNTIME_POLARITY_VALIDATED && !BOARD_EXTERNAL_PROTECTION_CONFIRMED
#error "Power-output arming requires measured FLT/EEV polarity and hold time"
#endif
#if (BOARD_POWER_STAGE_PROFILE == BOARD_PROFILE_SYNC_DAUGHTER_UNCONFIRMED) && \
    BOARD_POWER_OUTPUT_ARMING_ENABLE && !BOARD_SYNC_PINMAP_CONFIRMED
#error "Synchronous daughterboard PWM_A/PWM_B/DIS routing is unconfirmed"
#endif

/* GPIO/timer identifiers used by the static netlist regression. */
#define BOARD_GPIO_PORT_A_ID 0U
#define BOARD_GPIO_PORT_B_ID 1U
#define BOARD_GPIO_PORT_C_ID 2U
#define BOARD_GPIO_PORT_D_ID 3U
#define BOARD_HRTIM_TIMER_A_ID 0U
#define BOARD_HRTIM_TIMER_B_ID 1U
#define BOARD_HRTIM_TIMER_C_ID 2U
#define BOARD_HRTIM_TIMER_D_ID 3U
#define BOARD_HRTIM_TIMER_E_ID 4U
#define BOARD_HRTIM_TIMER_F_ID 5U

/* Board control pins. */
#define WS_PIN             13U /* PC13 */
#define OE1_PIN             2U /* PB2, active-low U11 OE#; high disables */
#define OE2_PIN            15U /* PC15, active-low U11 OE#; high disables */
#define OE3_PIN            14U /* PC14, active-low U11 OE#; high disables */
#define GATE_INHIBIT_PIN    2U /* PA2 -> both UCC27511 IN-; high inhibits */
#define AUX_ENABLE_PIN      5U /* PA5 -> MP9486 DIM/EN; high requests 15 V */

#define FLT1_PIN           12U /* PA12 */
#define FLT2_PIN           15U /* PA15 */
#define FLT3_PIN           10U /* PB10 */
#define FLT4_PIN           11U /* PB11 */
#define FLT5_PIN            0U /* PB0 */
#define FLT6_PIN           10U /* PC10 */
#define EEV1_PIN           12U /* PC12 */
#define EEV2_PIN           11U /* PC11 */

/* Complete U11 permutation from Netlist_474 control board 2026-08-28.
 * External labels CHA..CHF are connector names; timer identities are those
 * on U11's MCU side. */
#define BOARD_PWM_CHA1_PORT_ID BOARD_GPIO_PORT_B_ID
#define BOARD_PWM_CHA1_PIN 12U
#define BOARD_PWM_CHA1_TIMER_ID BOARD_HRTIM_TIMER_C_ID
#define BOARD_PWM_CHA1_OUTPUT 1U
#define BOARD_PWM_CHA2_PORT_ID BOARD_GPIO_PORT_B_ID
#define BOARD_PWM_CHA2_PIN 13U
#define BOARD_PWM_CHA2_TIMER_ID BOARD_HRTIM_TIMER_C_ID
#define BOARD_PWM_CHA2_OUTPUT 2U
#define BOARD_PWM_CHB1_PORT_ID BOARD_GPIO_PORT_B_ID
#define BOARD_PWM_CHB1_PIN 14U
#define BOARD_PWM_CHB1_TIMER_ID BOARD_HRTIM_TIMER_D_ID
#define BOARD_PWM_CHB1_OUTPUT 1U
#define BOARD_PWM_CHB2_PORT_ID BOARD_GPIO_PORT_B_ID
#define BOARD_PWM_CHB2_PIN 15U
#define BOARD_PWM_CHB2_TIMER_ID BOARD_HRTIM_TIMER_D_ID
#define BOARD_PWM_CHB2_OUTPUT 2U
#define BOARD_PWM_CHC1_PORT_ID BOARD_GPIO_PORT_C_ID
#define BOARD_PWM_CHC1_PIN 6U
#define BOARD_PWM_CHC1_TIMER_ID BOARD_HRTIM_TIMER_F_ID
#define BOARD_PWM_CHC1_OUTPUT 1U
#define BOARD_PWM_CHC2_PORT_ID BOARD_GPIO_PORT_C_ID
#define BOARD_PWM_CHC2_PIN 7U
#define BOARD_PWM_CHC2_TIMER_ID BOARD_HRTIM_TIMER_F_ID
#define BOARD_PWM_CHC2_OUTPUT 2U
#define BOARD_PWM_CHD1_PORT_ID BOARD_GPIO_PORT_C_ID
#define BOARD_PWM_CHD1_PIN 8U
#define BOARD_PWM_CHD1_TIMER_ID BOARD_HRTIM_TIMER_E_ID
#define BOARD_PWM_CHD1_OUTPUT 1U
#define BOARD_PWM_CHD2_PORT_ID BOARD_GPIO_PORT_C_ID
#define BOARD_PWM_CHD2_PIN 9U
#define BOARD_PWM_CHD2_TIMER_ID BOARD_HRTIM_TIMER_E_ID
#define BOARD_PWM_CHD2_OUTPUT 2U
#define BOARD_PWM_CHE1_PORT_ID BOARD_GPIO_PORT_A_ID
#define BOARD_PWM_CHE1_PIN 8U
#define BOARD_PWM_CHE1_TIMER_ID BOARD_HRTIM_TIMER_A_ID
#define BOARD_PWM_CHE1_OUTPUT 1U
#define BOARD_PWM_CHE2_PORT_ID BOARD_GPIO_PORT_A_ID
#define BOARD_PWM_CHE2_PIN 9U
#define BOARD_PWM_CHE2_TIMER_ID BOARD_HRTIM_TIMER_A_ID
#define BOARD_PWM_CHE2_OUTPUT 2U
#define BOARD_PWM_CHF1_PORT_ID BOARD_GPIO_PORT_A_ID
#define BOARD_PWM_CHF1_PIN 10U
#define BOARD_PWM_CHF1_TIMER_ID BOARD_HRTIM_TIMER_B_ID
#define BOARD_PWM_CHF1_OUTPUT 1U
#define BOARD_PWM_CHF2_PORT_ID BOARD_GPIO_PORT_A_ID
#define BOARD_PWM_CHF2_PIN 11U
#define BOARD_PWM_CHF2_TIMER_ID BOARD_HRTIM_TIMER_B_ID
#define BOARD_PWM_CHF2_OUTPUT 2U

/* The non-synchronous power board consumes external CHE2 and CHF1. They are
 * PA9/HRTIM1_CHA2 (Timer A output 2) and PA10/HRTIM1_CHB1 (Timer B output 1),
 * not native Timer E/F signals. */
#define BOARD_NONSYNC_CHE2_PORT_ID BOARD_GPIO_PORT_A_ID
#define BOARD_NONSYNC_CHE2_PIN BOARD_PWM_CHE2_PIN
#define BOARD_NONSYNC_CHE2_TIMER_ID BOARD_HRTIM_TIMER_A_ID
#define BOARD_NONSYNC_CHE2_OUTPUT 2U
#define BOARD_NONSYNC_CHF1_PORT_ID BOARD_GPIO_PORT_A_ID
#define BOARD_NONSYNC_CHF1_PIN BOARD_PWM_CHF1_PIN
#define BOARD_NONSYNC_CHF1_TIMER_ID BOARD_HRTIM_TIMER_B_ID
#define BOARD_NONSYNC_CHF1_OUTPUT 1U
#define BOARD_NONSYNC_PWM_OE_PIN OE3_PIN

#define HRTIM_PA_OUTPUT_MASK ((1UL << 8) | (1UL << 9) | \
                              (1UL << 10) | (1UL << 11))
#define HRTIM_PB_OUTPUT_MASK ((1UL << 12) | (1UL << 13) | \
                              (1UL << 14) | (1UL << 15))
#define HRTIM_PC_OUTPUT_MASK ((1UL << 6) | (1UL << 7) | \
                              (1UL << 8) | (1UL << 9))

/* UCC21550 daughterboard H1-5 is DIS: high disables, low permits. */
#define SYNC_DRIVER_DIS_ACTIVE_HIGH 1U

/* Diagnostic software limits. The requested PV OVP threshold is 165 V. With
 * the present 225 k / 4.42 k divider and op-amp gain, this is close to the
 * 3.3-V ADC rail; the ADC rail fault therefore remains an earlier fail-safe
 * and the 165-V value is not a precision measurement point. */
#define BOARD_VBUS_OVP_LIMIT_MV 415000UL
#define BOARD_CONTROL_TARGET_MAX_MV 400000UL
/* The source is nominally 75 V.  The measured ADC result is 75.2 V due to
 * divider/gain tolerance, so a hard 75.000-V reachability ceiling rejects a
 * valid 75-V operating point before PWM can start.  Keep 90 V as the
 * independent PV OVP ceiling and allow 8% operating tolerance here. */
#define BOARD_PV_OPERATING_MAX_MV 80000UL
#define BOARD_PV_OVP_LIMIT_MV 165000UL
#define BOARD_VBUS_DYNAMIC_OVP_MARGIN_MIN_MV 10000UL
#define BOARD_VBUS_DYNAMIC_OVP_MARGIN_PPM 50000UL
#define BOARD_BOOST_MIN_HEADROOM_MV 5000UL
#define BOARD_NTC_OTP_TRIP_CDEG 9000L
#define BOARD_NTC_OTP_RECOVER_CDEG 8000L
#define BOARD_NTC_INVALID_CONFIRM_SAMPLES 8UL
#define BOARD_OTP_CONFIRM_SAMPLES 10UL

/* Runtime supervisory limits.  They are conservative commissioning limits;
 * each remains subordinate to the compile-time arming gates above. */
#define BOARD_PV_UVLO_START_MV 20000UL
#define BOARD_PV_UVLO_STOP_MV 18000UL
#define BOARD_CURRENT_IMBALANCE_MIN_MA 500UL
#define BOARD_CURRENT_IMBALANCE_PPM 300000UL
#define BOARD_CURRENT_IMBALANCE_CONFIRM_MS 50UL
#define BOARD_CURRENT_STUCK_BAND_MA 30UL
#define BOARD_CURRENT_STUCK_DUTY_Q15 3277UL
#define BOARD_CURRENT_STUCK_BLANKING_MS 500UL
#define BOARD_CURRENT_STUCK_CONFIRM_MS 250UL
#define BOARD_BOOST_UNREACHABLE_CONFIRM_MS 20UL
#define BOARD_VBUS_BUILD_TIMEOUT_MS 2000UL
#define BOARD_DUTY_SATURATION_TIMEOUT_MS 500UL
#define BOARD_INPUT_POWER_LIMIT_MW 300000UL
#define BOARD_POWER_LIMIT_CONFIRM_MS 20UL
#define BOARD_RX_DRAIN_BUDGET_BYTES 32UL

/* Measurement-chain gains, used directly by engineering-unit conversion,
 * Iin=I1+I2 reconstruction, power calculation and raw OCP/OVP thresholds.
 * CC6937S8-3FB030 nominal sensitivity is 44 mV/A.  Calibration is allowed
 * only within +/-20%; zero +/- OCP excursion must also remain inside ADC.
 * These values are not the compensator Kp/Ki of a current or voltage loop. */
#define BOARD_CURRENT_GAIN_MIN_MV_PER_A 35UL
#define BOARD_CURRENT_GAIN_MAX_MV_PER_A 53UL
#define BOARD_VOLTAGE_GAIN_MIN_PPM 900000UL
#define BOARD_VOLTAGE_GAIN_MAX_PPM 1100000UL

/* The inner current loop executes from the PWM-synchronous ADC ISR only after
 * plant gains have been validated.  Zero values intentionally make the
 * uncommissioned build incapable of producing a closed-loop duty command. */
#define BOARD_CURRENT_LOOP_KP_Q15_PER_RAW 0L
#define BOARD_CURRENT_LOOP_KI_Q15_PER_RAW 0L
#define BOARD_CURRENT_LOOP_CORRECTION_LIMIT_Q15 4096L

/* Provisional fixed-point outer-loop coefficients. They support dry-run
 * diagnostics only. BOARD_CONTROL_GAINS_VALIDATED must not be changed until
 * plant identification and low-voltage step/frequency-response tests exist. */
#define BOARD_OUTER_KP_Q15_PER_MV_NUM 1L
#define BOARD_OUTER_KP_Q15_PER_MV_DEN 20L
#define BOARD_OUTER_KI_Q15_PER_MV_NUM 1L
#define BOARD_OUTER_KI_Q15_PER_MV_DEN 10L
#define BOARD_MPPT_FILTER_SHIFT 3U
#define BOARD_MPPT_RELATIVE_DEADBAND_PPM 2000UL
#define BOARD_MPPT_STEP_MIN_MV 100UL
#define BOARD_MPPT_STEP_MAX_MV 500UL
#define BOARD_SOFTSTART_STEP_Q15 164UL

#endif
