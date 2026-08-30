#include "power_control.h"
#include "stm32g474_bare.h"
#include "board_hrtim.h"
#include "board_safety.h"
#include "board_config.h"
#include "board_fast_adc.h"
#include <stdint.h>

__attribute__((weak)) void power_control_watchdog_kick(void)
{
}

/* Nominal measurement constants retained from the earlier power-board
 * measurement design and checked against the supplied 2026-08-28
 * non-synchronous half-bridge small-board netlist.
 * They support commissioning telemetry, not end-to-end calibration. */
#define ADC_FULL_SCALE_MV 3300UL
#define ADC_FULL_SCALE_CODE 4095UL
#define ADC_CONVERSION_TIMEOUT 50000UL
#define CONTROL_SCAN_PERIOD_MS 1UL
#define CONTROL_FAST_PERIOD_MS 1UL
#define CONTROL_OUTER_PERIOD_MS 1UL
#define CONTROL_MPPT_PERIOD_MS 100UL
#define CONTROL_DUTY_MIN_Q15 655UL
#define CONTROL_DUTY_MAX_Q15 27852UL
#define CONTROL_DEFAULT_TARGET_MV 200000UL
#define PV_DIV_TOP_OHM 225000UL
#define PV_DIV_BOTTOM_OHM 4420UL
#define PV_NEGATIVE_LEG_OHM 300000UL
#define PV_FEEDBACK_OHM 9310UL
#define VBUS_DIV_TOP_OHM 882000UL
#define VBUS_DIV_BOTTOM_OHM 5100UL
#define CURRENT_SENSOR_NOMINAL_MV_PER_A 44UL
#define VBUS_OVP_LIMIT_MV BOARD_VBUS_OVP_LIMIT_MV
#define PV_OVP_LIMIT_MV BOARD_PV_OVP_LIMIT_MV
#define CURRENT_OCP_LIMIT_MA 2500UL
#define AUX_UVLO_LIMIT_MV 15000UL
#define OVP_CONFIRM_SAMPLES 2UL
#define OCP_CONFIRM_SAMPLES 2UL
#define FAST_ADC_STALL_TIMEOUT_MS 5UL
#define CURRENT_SENSOR_LOW_RAW 8U
#define MPPT_POWER_DEADBAND_MIN_MW 50UL
#define VBUS_LIMIT_RELEASE_HYST_MV 2000UL
#define CURRENT_OPERATING_LIMIT_MA 2000UL

typedef struct { ADC_TypeDef *adc; uint8_t channel; uint8_t index; } adc_channel_map_t;

/* PA0/PA1 are the two current-conditioner outputs; their physical direction
 * is intentionally reported as I1/I2 until a signed-current calibration is
 * measured on the assembled power board. PC0/PC1 are the confirmed PV/VBUS
 * voltage channels. PA2 is omitted because the power-board connector uses it
 * for the shared UCC27511 IN- inhibit path. */
static const adc_channel_map_t adc_map[POWER_ADC_CHANNEL_COUNT] = {
    {ADC1, 1U, POWER_ADC_I1}, {ADC1, 2U, POWER_ADC_I2},
    {ADC1, 4U, POWER_ADC_NTC1}, {ADC2, 3U, POWER_ADC_NTC2},
    {ADC2, 4U, POWER_ADC_AUX0}, {ADC2, 5U, POWER_ADC_AUX1},
    {ADC2, 6U, POWER_ADC_VPV}, {ADC2, 7U, POWER_ADC_VBUS},
    {ADC3, 1U, POWER_ADC_AUX2}
};

static power_sample_t sample;
static power_control_status_t control;
static uint32_t schedule_started;
static uint32_t next_scan;
static uint32_t next_fast;
static uint32_t next_outer;
static uint32_t next_mppt;
static uint32_t next_fast_rate;
static uint32_t previous_fast_sequence;
static uint32_t previous_fast_rate_ms;
static uint32_t watched_fast_sequence;
static uint32_t last_fast_progress_ms;
static int32_t outer_integrator_q15;
static int32_t previous_power_mw;
static int32_t mppt_direction = -1;
static uint32_t filter_initialized;
static uint32_t adc_ready_mask;
static uint32_t vbus_ovp_samples;
static uint32_t pv_ovp_samples;
static uint32_t ocp_samples;
static uint32_t ntc1_invalid_samples;
static uint32_t ntc2_invalid_samples;
static uint32_t ntc1_otp_samples;
static uint32_t ntc2_otp_samples;
static uint32_t current_zero_i1_mv;
static uint32_t current_zero_i2_mv;
static uint32_t current_gain_i1_mv_per_a;
static uint32_t current_gain_i2_mv_per_a;
static int32_t current_polarity_i1;
static int32_t current_polarity_i2;
static uint32_t pv_gain_ppm;
static uint32_t vbus_gain_ppm;
static uint32_t external_fault_latched;
static uint32_t last_poll_ms;
static uint32_t mode_start_ms;
static uint32_t current_imbalance_ms;
static uint32_t current_stuck_ms;
static uint32_t boost_unreachable_ms;
static uint32_t duty_saturation_ms;
static uint32_t power_limit_ms;
static uint32_t previous_mppt_sample_sequence;
static uint32_t last_stop_fault_bits;
static uint32_t last_stop_fast_fault_flags;
static uint32_t last_stop_duty_q15;
static uint32_t last_stop_time_ms;

#define POWER_REQUIRED_VALID_MASK \
    ((1UL << POWER_ADC_I1) | (1UL << POWER_ADC_I2) | \
     (1UL << POWER_ADC_VPV) | (1UL << POWER_ADC_VBUS))
#define POWER_CRITICAL_FAULT_MASK \
    (POWER_FAULT_ADC_INIT | POWER_FAULT_ADC_TIMEOUT | POWER_FAULT_ADC_RAIL | \
     POWER_FAULT_HRTIM_LOCK | \
     POWER_FAULT_PV_OVP | POWER_FAULT_VBUS_OVP | POWER_FAULT_OCP | \
     POWER_FAULT_15V_UVLO | POWER_FAULT_AUX_DROP | \
     POWER_FAULT_OTP_NTC1 | POWER_FAULT_OTP_NTC2 | \
     POWER_FAULT_NTC_SENSOR | POWER_FAULT_CURRENT_SENSOR | \
     POWER_FAULT_BOOST_UNREACHABLE | POWER_FAULT_PV_UVLO | \
     POWER_FAULT_CURRENT_IMBALANCE | POWER_FAULT_CURRENT_STUCK | \
     POWER_FAULT_VBUS_BUILD_TIMEOUT | POWER_FAULT_DUTY_SATURATION | \
     POWER_FAULT_POWER_LIMIT | POWER_FAULT_SAMPLE_STALE)

static void update_calibration_state(void)
{
    control.calibration_valid =
        (control.current_calibration_valid != 0UL &&
         control.current_polarity_valid != 0UL &&
         control.voltage_calibration_valid != 0UL) ? 1UL : 0UL;
    if (control.calibration_valid != 0UL)
        control.fault_bits &= ~POWER_FAULT_CALIBRATION_REQUIRED;
    else
        control.fault_bits |= POWER_FAULT_CALIBRATION_REQUIRED;
}

static uint32_t abs_i32_u32(int32_t value)
{
    return (value < 0) ? (uint32_t)(-(value + 1)) + 1UL : (uint32_t)value;
}

static int32_t iir_shift(int32_t previous, int32_t input, uint32_t shift)
{
    int64_t delta = (int64_t)input - previous;
    if (delta >= 0) delta += (int64_t)1 << (shift - 1U);
    else delta -= (int64_t)1 << (shift - 1U);
    return previous + (int32_t)(delta / ((int64_t)1 << shift));
}

static uint32_t dynamic_vbus_ovp_limit_mv(void)
{
    uint32_t margin = (uint32_t)(((uint64_t)control.target_mv *
                                  BOARD_VBUS_DYNAMIC_OVP_MARGIN_PPM +
                                  500000ULL) / 1000000ULL);
    uint32_t limit;
    if (margin < BOARD_VBUS_DYNAMIC_OVP_MARGIN_MIN_MV)
        margin = BOARD_VBUS_DYNAMIC_OVP_MARGIN_MIN_MV;
    limit = control.target_mv + margin;
    if (limit < control.target_mv || limit > VBUS_OVP_LIMIT_MV)
        limit = VBUS_OVP_LIMIT_MV;
    return limit;
}

static uint32_t apply_gain_ppm(uint32_t value, uint32_t gain_ppm)
{
    uint64_t scaled = (uint64_t)value * gain_ppm;
    return (uint32_t)((scaled + 500000ULL) / 1000000ULL);
}

static void contain_critical_faults(void)
{
    /* Runtime measurement faults must never convert SAFE_OFF into a latched
     * power fault.  They become containment events only after a run has been
     * requested or outputs are physically enabled. */
    if ((control.mode != POWER_CONTROL_OFF || control.outputs_enabled != 0UL) &&
        (control.fault_bits & POWER_CRITICAL_FAULT_MASK) != 0UL) {
        last_stop_fault_bits = control.fault_bits & POWER_CRITICAL_FAULT_MASK;
        last_stop_fast_fault_flags = board_fast_adc_fault_flags();
        last_stop_duty_q15 = control.duty_q15;
        last_stop_time_ms = last_poll_ms;
        control.last_stop_fault_bits = last_stop_fault_bits;
        control.last_stop_fast_fault_flags = last_stop_fast_fault_flags;
        control.last_stop_duty_q15 = last_stop_duty_q15;
        control.last_stop_time_ms = last_stop_time_ms;
        control.mode = POWER_CONTROL_OFF;
        control.duty_q15 = 0UL;
        control.proposed_duty_q15 = 0UL;
        control.outputs_enabled = 0UL;
        board_safety_force_power_off();
    }
}

static void update_io_status(void)
{
    control.aux_enable = (GPIOA->ODR >> AUX_ENABLE_PIN) & 1UL;
    control.gate_inhibit = (GPIOA->ODR >> GATE_INHIBIT_PIN) & 1UL;
}

static uint32_t boost_input_is_reachable(void)
{
    if (control.pv_mv <= 0 || (uint32_t)control.pv_mv >
        BOARD_PV_OPERATING_MAX_MV) return 0UL;
    return (control.target_mv >= (uint32_t)control.pv_mv +
            BOARD_BOOST_MIN_HEADROOM_MV) ? 1UL : 0UL;
}

static void enforce_boost_reachability(void)
{
    if (control.mode == POWER_CONTROL_OFF) {
        control.boost_reachable = 0UL;
        boost_unreachable_ms = 0UL;
        return;
    }
    control.boost_reachable = boost_input_is_reachable();
    if (control.boost_reachable == 0UL) {
        if (boost_unreachable_ms < BOARD_BOOST_UNREACHABLE_CONFIRM_MS)
            ++boost_unreachable_ms;
        if (boost_unreachable_ms >= BOARD_BOOST_UNREACHABLE_CONFIRM_MS) {
            control.fault_bits |= POWER_FAULT_BOOST_UNREACHABLE;
            contain_critical_faults();
        }
    } else {
        boost_unreachable_ms = 0UL;
        control.fault_bits &= ~POWER_FAULT_BOOST_UNREACHABLE;
    }
}

static void update_runtime_supervision(void)
{
    uint32_t imbalance_limit;
    uint32_t average_current;
    uint32_t elapsed;
    if (control.mode == POWER_CONTROL_OFF) {
        current_imbalance_ms = 0UL;
        current_stuck_ms = 0UL;
        boost_unreachable_ms = 0UL;
        duty_saturation_ms = 0UL;
        power_limit_ms = 0UL;
        control.vbus_build_elapsed_ms = 0UL;
        return;
    }

    if (control.pv_mv < (int32_t)BOARD_PV_UVLO_STOP_MV)
        control.fault_bits |= POWER_FAULT_PV_UVLO;

    elapsed = last_poll_ms - mode_start_ms;

    average_current = (abs_i32_u32(control.i1_ma) +
                       abs_i32_u32(control.i2_ma)) / 2UL;
    imbalance_limit = (uint32_t)(((uint64_t)average_current *
        BOARD_CURRENT_IMBALANCE_PPM + 500000ULL) / 1000000ULL);
    if (imbalance_limit < BOARD_CURRENT_IMBALANCE_MIN_MA)
        imbalance_limit = BOARD_CURRENT_IMBALANCE_MIN_MA;
    if (abs_i32_u32(control.i1_ma - control.i2_ma) > imbalance_limit) {
        if (current_imbalance_ms < BOARD_CURRENT_IMBALANCE_CONFIRM_MS)
            ++current_imbalance_ms;
    } else current_imbalance_ms = 0UL;
    if (current_imbalance_ms >= BOARD_CURRENT_IMBALANCE_CONFIRM_MS)
        control.fault_bits |= POWER_FAULT_CURRENT_IMBALANCE;

    /* Zero current is normal while the MCU is up but the external source or
     * PWM bank is still off.  Treat it as a plant diagnostic only after the
     * physical HRTIM/OE path has been confirmed, the soft-start blanking time
     * has elapsed and a meaningful duty command is actually applied. */
    if (control.outputs_enabled != 0UL &&
        elapsed >= BOARD_CURRENT_STUCK_BLANKING_MS &&
        control.duty_q15 >= BOARD_CURRENT_STUCK_DUTY_Q15 &&
        control.vbus_mv < (int32_t)(control.target_mv * 9UL / 10UL) &&
        abs_i32_u32(control.pv_current_ma) < BOARD_CURRENT_STUCK_BAND_MA) {
        if (current_stuck_ms < BOARD_CURRENT_STUCK_CONFIRM_MS)
            ++current_stuck_ms;
    } else current_stuck_ms = 0UL;
    if (current_stuck_ms >= BOARD_CURRENT_STUCK_CONFIRM_MS)
        control.fault_bits |= POWER_FAULT_CURRENT_STUCK;

    control.vbus_build_elapsed_ms = elapsed;
    if (control.outputs_enabled != 0UL &&
        elapsed >= BOARD_VBUS_BUILD_TIMEOUT_MS &&
        control.vbus_mv < (int32_t)(control.target_mv * 9UL / 10UL))
        control.fault_bits |= POWER_FAULT_VBUS_BUILD_TIMEOUT;

    if (control.outputs_enabled != 0UL &&
        control.proposed_duty_q15 >= CONTROL_DUTY_MAX_Q15 &&
        control.vbus_mv < (int32_t)(control.target_mv * 95UL / 100UL)) {
        if (duty_saturation_ms < BOARD_DUTY_SATURATION_TIMEOUT_MS)
            ++duty_saturation_ms;
    } else duty_saturation_ms = 0UL;
    if (duty_saturation_ms >= BOARD_DUTY_SATURATION_TIMEOUT_MS)
        control.fault_bits |= POWER_FAULT_DUTY_SATURATION;

    if (control.outputs_enabled != 0UL &&
        control.pv_power_mw > (int32_t)BOARD_INPUT_POWER_LIMIT_MW) {
        if (power_limit_ms < BOARD_POWER_LIMIT_CONFIRM_MS) ++power_limit_ms;
    } else power_limit_ms = 0UL;
    if (power_limit_ms >= BOARD_POWER_LIMIT_CONFIRM_MS)
        control.fault_bits |= POWER_FAULT_POWER_LIMIT;

    control.current_imbalance_ms = current_imbalance_ms;
    control.current_stuck_ms = current_stuck_ms;
    control.duty_saturation_ms = duty_saturation_ms;
    control.power_limit_ms = power_limit_ms;
    control.current_limit_active =
        (abs_i32_u32(control.i1_ma) >= CURRENT_OPERATING_LIMIT_MA ||
         abs_i32_u32(control.i2_ma) >= CURRENT_OPERATING_LIMIT_MA) ? 1UL : 0UL;
}

static int32_t saturate_i64_to_i32(int64_t value)
{
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

static void analog_pin(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t shift = pin * 2U;
    port->MODER |= 3UL << shift;
    port->PUPDR &= ~(3UL << shift);
}

static void adc_sample_time(ADC_TypeDef *adc, uint32_t channel)
{
    if (channel <= 9UL) {
        adc->SMPR1 = (adc->SMPR1 & ~(7UL << (channel * 3UL))) |
                     (7UL << (channel * 3UL));
    } else {
        channel -= 10UL;
        adc->SMPR2 = (adc->SMPR2 & ~(7UL << (channel * 3UL))) |
                     (7UL << (channel * 3UL));
    }
}

static uint32_t adc_enable(ADC_TypeDef *adc)
{
    uint32_t timeout = ADC_CONVERSION_TIMEOUT;
    adc->CR &= ~(ADC_CR_DEEPPWD | ADC_CR_ADEN);
    adc->CR |= ADC_CR_ADVREGEN;
    while (timeout-- != 0UL) { __asm volatile ("nop"); }
    adc->CR |= ADC_CR_ADCAL;
    timeout = ADC_CONVERSION_TIMEOUT;
    while ((adc->CR & ADC_CR_ADCAL) != 0UL && timeout-- != 0UL) {}
    if ((adc->CR & ADC_CR_ADCAL) != 0UL) return 0UL;
    adc->ISR = ADC_ISR_ADRDY | ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
    adc->CR |= ADC_CR_ADEN;
    timeout = ADC_CONVERSION_TIMEOUT;
    while ((adc->ISR & ADC_ISR_ADRDY) == 0UL && timeout-- != 0UL) {}
    return ((adc->ISR & ADC_ISR_ADRDY) != 0UL) ? 1UL : 0UL;
}

static uint32_t adc_read_single(ADC_TypeDef *adc, uint32_t channel, uint16_t *result)
{
    uint32_t timeout = ADC_CONVERSION_TIMEOUT;
    if ((adc->CR & ADC_CR_ADEN) == 0UL || result == (uint16_t *)0) return 0UL;
    adc->SQR1 = (adc->SQR1 & ~(0x1FUL << 6)) | ((channel & 0x1FUL) << 6);
    adc->ISR = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
    adc->CR |= ADC_CR_ADSTART;
    while ((adc->ISR & ADC_ISR_EOC) == 0UL && timeout-- != 0UL) {
        /* A stalled ADC must return a bounded error, but it must not turn a
         * diagnostic calibration command into an unexplained IWDG reset while
         * the timeout loop is being exhausted. */
        if ((timeout & 0x3FFUL) == 0UL) power_control_watchdog_kick();
    }
    if ((adc->ISR & ADC_ISR_EOC) == 0UL) return 0UL;
    *result = (uint16_t)(adc->DR & ADC_FULL_SCALE_CODE);
    return 1UL;
}

static uint16_t raw_to_pin_mv(uint16_t raw)
{
    return (uint16_t)(((uint32_t)raw * ADC_FULL_SCALE_MV + 2047UL) /
                       ADC_FULL_SCALE_CODE);
}

static uint32_t vbus_input_mv_to_raw(uint32_t input_mv)
{
    uint64_t numerator = (uint64_t)input_mv *
                         VBUS_DIV_BOTTOM_OHM * ADC_FULL_SCALE_CODE;
    uint64_t denominator = (uint64_t)(VBUS_DIV_TOP_OHM + VBUS_DIV_BOTTOM_OHM) *
                           ADC_FULL_SCALE_MV;
    return (uint32_t)((numerator + denominator / 2ULL) / denominator);
}

static uint32_t pv_input_mv_to_raw(uint32_t input_mv)
{
    uint64_t numerator = (uint64_t)input_mv * PV_DIV_BOTTOM_OHM *
                         (PV_NEGATIVE_LEG_OHM + PV_FEEDBACK_OHM) *
                         ADC_FULL_SCALE_CODE;
    uint64_t denominator = (uint64_t)(PV_DIV_TOP_OHM + PV_DIV_BOTTOM_OHM) *
                           PV_NEGATIVE_LEG_OHM * ADC_FULL_SCALE_MV;
    return (uint32_t)((numerator + denominator / 2ULL) / denominator);
}

static uint16_t pin_mv_to_raw(uint32_t pin_mv)
{
    uint32_t raw = (pin_mv * ADC_FULL_SCALE_CODE +
                    ADC_FULL_SCALE_MV / 2UL) / ADC_FULL_SCALE_MV;
    return (uint16_t)((raw > ADC_FULL_SCALE_CODE) ?
                      ADC_FULL_SCALE_CODE : raw);
}

static void update_fast_adc_limits(void)
{
    uint32_t i1_delta_pin_mv = 0UL, i2_delta_pin_mv = 0UL;
    uint32_t vbus_raw;
    uint32_t pv_raw = pv_input_mv_to_raw(PV_OVP_LIMIT_MV);
    control.vbus_dynamic_ovp_limit_mv = dynamic_vbus_ovp_limit_mv();
    vbus_raw = vbus_input_mv_to_raw(control.vbus_dynamic_ovp_limit_mv);
    if (control.current_calibration_valid != 0UL) {
        i1_delta_pin_mv = (current_gain_i1_mv_per_a * CURRENT_OCP_LIMIT_MA +
                           500UL) / 1000UL;
        i2_delta_pin_mv = (current_gain_i2_mv_per_a * CURRENT_OCP_LIMIT_MA +
                           500UL) / 1000UL;
    }
    if (control.voltage_calibration_valid != 0UL && vbus_gain_ppm != 0UL) {
        vbus_raw = (uint32_t)(((uint64_t)vbus_raw * 1000000ULL +
                              vbus_gain_ppm / 2UL) / vbus_gain_ppm);
        pv_raw = (uint32_t)(((uint64_t)pv_raw * 1000000ULL +
                            pv_gain_ppm / 2UL) / pv_gain_ppm);
    }
    if (vbus_raw > ADC_FULL_SCALE_CODE) vbus_raw = ADC_FULL_SCALE_CODE;
    if (pv_raw > ADC_FULL_SCALE_CODE) pv_raw = ADC_FULL_SCALE_CODE;
    control.vbus_ovp_raw_threshold = vbus_raw;
    board_fast_adc_set_limits(
        pin_mv_to_raw(current_zero_i1_mv),
        pin_mv_to_raw(current_zero_i2_mv),
        pin_mv_to_raw(i1_delta_pin_mv),
        pin_mv_to_raw(i2_delta_pin_mv),
        (uint16_t)pv_raw,
        (uint16_t)vbus_raw,
        control.current_calibration_valid);
}

static void resume_fast_sampling(void)
{
#if BOARD_PER_CYCLE_ADC_ENABLE
    board_fast_adc_snapshot_t first = {0};
    if ((adc_ready_mask & 0x3UL) == 0x3UL &&
        board_hrtim_backend_ready() != 0UL &&
        board_fast_adc_start() != 0UL) {
        control.per_cycle_sampling_running = 1UL;
        control.fault_bits &= ~POWER_FAULT_HRTIM_LOCK;
        if (board_fast_adc_get_snapshot(&first) != 0UL) {
            watched_fast_sequence = first.sequence;
            last_fast_progress_ms = last_poll_ms;
        }
    } else {
        if (board_fast_adc_get_snapshot(&first) != 0UL ||
            first.start_failure_count != 0UL) {
            control.fast_sample_sequence = first.sequence;
            control.fast_adc_irq_count = first.irq_count;
            control.fast_adc_incomplete_count = first.incomplete_count;
            control.fast_adc_fault_flags = first.fault_flags;
        }
        control.per_cycle_sampling_running = 0UL;
        control.fault_bits |= POWER_FAULT_HRTIM_LOCK;
    }
#endif
}

static uint32_t pin_to_engineering_mv(uint16_t pin_mv, uint32_t top_ohm,
                                      uint32_t bottom_ohm)
{
    uint64_t numerator = (uint64_t)pin_mv * (top_ohm + bottom_ohm);
    return (uint32_t)((numerator + bottom_ohm / 2ULL) / bottom_ohm);
}

static uint32_t pv_pin_to_input_mv(uint16_t pin_mv)
{
    /* U3B: 225 k / 4.42 k positive divider followed by a non-inverting
     * gain of 1 + 9.31 k / 300 k. Assumes PGND and ADC GND are at the same
     * DC potential; PCB ground drop remains part of end-to-end calibration. */
    uint64_t numerator = (uint64_t)pin_mv *
                         (PV_DIV_TOP_OHM + PV_DIV_BOTTOM_OHM) *
                         PV_NEGATIVE_LEG_OHM;
    uint64_t denominator = (uint64_t)PV_DIV_BOTTOM_OHM *
                           (PV_NEGATIVE_LEG_OHM + PV_FEEDBACK_OHM);
    return (uint32_t)((numerator + denominator / 2ULL) / denominator);
}

static uint32_t ntc_raw_to_cdeg(uint16_t raw, int32_t *temperature_cdeg)
{
    /* MT52B103F3950F01000 (10 k, B25/50=3950 K) above a 4.42 k resistor.
     * Values are ratiometric ADC codes at 5 C intervals. Linear interpolation
     * avoids floating point in the target while retaining a clear validity
     * range. This is a nominal estimate until thermal calibration is done. */
    static const uint16_t ntc_raw_table[] = {
        45U, 63U, 88U, 122U, 165U, 220U, 289U, 374U, 476U, 596U,
        736U, 894U, 1068U, 1255U, 1453U, 1657U, 1862U, 2064U,
        2260U, 2446U, 2621U, 2782U, 2929U, 3062U, 3181U, 3287U,
        3381U, 3464U, 3537U, 3601U, 3657U, 3706U, 3750U, 3788U
    };
    uint32_t i;
    uint32_t count = sizeof(ntc_raw_table) / sizeof(ntc_raw_table[0]);
    if (temperature_cdeg == (int32_t *)0 || raw < ntc_raw_table[0] ||
        raw > ntc_raw_table[count - 1UL]) return 0UL;
    for (i = 1UL; i < count; ++i) {
        if (raw <= ntc_raw_table[i]) {
            uint32_t span = ntc_raw_table[i] - ntc_raw_table[i - 1UL];
            uint32_t offset = ((uint32_t)(raw - ntc_raw_table[i - 1UL]) *
                               500UL + span / 2UL) / span;
            *temperature_cdeg = -4000 + (int32_t)((i - 1UL) * 500UL + offset);
            return 1UL;
        }
    }
    return 0UL;
}

static uint32_t adc_ready_for(ADC_TypeDef *adc)
{
    if (adc == ADC1) return 1UL << 0;
    if (adc == ADC2) return 1UL << 1;
    if (adc == ADC3) return 1UL << 2;
    return 0UL;
}

static uint32_t adc_backend_init(void)
{
    uint32_t i;
    uint32_t ok;
    RCC_AHB2ENR |= RCC_AHB2ENR_ADC12EN | RCC_AHB2ENR_ADC345EN;
    /* A debugger core reset does not necessarily return ADC state to reset.
     * Pulse both ADC reset domains before regulator/calibration sequencing. */
    RCC_AHB2RSTR |= RCC_AHB2RSTR_ADC12RST | RCC_AHB2RSTR_ADC345RST;
    RCC_AHB2RSTR &= ~(RCC_AHB2RSTR_ADC12RST | RCC_AHB2RSTR_ADC345RST);
    ADC12_COMMON->CCR = (ADC12_COMMON->CCR & ~ADC_CCR_CKMODE_Msk) |
                        ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1;
    ADC345_COMMON->CCR = (ADC345_COMMON->CCR & ~ADC_CCR_CKMODE_Msk) |
                         ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1;
    analog_pin(GPIOA, 0U); analog_pin(GPIOA, 1U); analog_pin(GPIOA, 3U);
    analog_pin(GPIOA, 6U); analog_pin(GPIOA, 7U); analog_pin(GPIOB, 1U);
    analog_pin(GPIOC, 0U); analog_pin(GPIOC, 1U); analog_pin(GPIOC, 4U);
    ADC1->CFGR = 0UL; ADC2->CFGR = 0UL; ADC3->CFGR = 0UL;
    ADC1->DIFSEL = 0UL; ADC2->DIFSEL = 0UL; ADC3->DIFSEL = 0UL;
    ADC1->SQR1 = 0UL; ADC2->SQR1 = 0UL; ADC3->SQR1 = 0UL;
    for (i = 0UL; i < POWER_ADC_CHANNEL_COUNT; ++i) {
        adc_sample_time(adc_map[i].adc, adc_map[i].channel);
    }
    ok = 0UL;
    if (adc_enable(ADC1) != 0UL) ok |= 1UL << 0;
    if (adc_enable(ADC2) != 0UL) ok |= 1UL << 1;
    if (adc_enable(ADC3) != 0UL) ok |= 1UL << 2;
    return ok;
}

static uint32_t adc_scan(void)
{
    uint32_t i;
    uint32_t ok = 1UL;
    uint32_t fast_running = board_hrtim_sampling_is_running();
    board_fast_adc_snapshot_t fast;
    uint32_t fast_valid = board_fast_adc_get_snapshot(&fast);
    sample.valid_mask = 0UL;
    sample.fault_bits = 0UL;
    for (i = 0UL; i < POWER_ADC_CHANNEL_COUNT; ++i) {
        uint16_t raw = 0U;
        sample.raw[i] = 0U;
        sample.pin_mv[i] = 0U;
        if ((adc_ready_mask & adc_ready_for(adc_map[i].adc)) == 0UL) {
            ok = 0UL;
            sample.fault_bits |= POWER_FAULT_ADC_INIT;
            continue;
        }
        if (fast_valid != 0UL && fast_running != 0UL &&
            i == POWER_ADC_I1) raw = fast.i1_raw;
        else if (fast_valid != 0UL && fast_running != 0UL &&
                 i == POWER_ADC_I2) raw = fast.i2_raw;
        else if (fast_valid != 0UL && fast_running != 0UL &&
                 i == POWER_ADC_NTC1) raw = fast.ntc1_raw;
        else if (fast_valid != 0UL && fast_running != 0UL &&
                 i == POWER_ADC_NTC2) raw = fast.ntc2_raw;
        else if (fast_valid != 0UL && fast_running != 0UL &&
                 i == POWER_ADC_VPV) raw = fast.vpv_raw;
        else if (fast_valid != 0UL && fast_running != 0UL &&
                 i == POWER_ADC_VBUS) raw = fast.vbus_raw;
        else if (fast_running != 0UL &&
                 (adc_map[i].adc == ADC1 || adc_map[i].adc == ADC2)) {
            /* PA7/PC4 are unused auxiliary channels. Do not launch a regular
             * conversion on ADC1/2 while their injected queues run at 100 kHz.
             * They remain explicitly invalid and cannot create a fatal ADC
             * timeout for the active control channels. */
            ok = 0UL;
            continue;
        }
        else if (adc_read_single(adc_map[i].adc, adc_map[i].channel, &raw) == 0UL) {
            ok = 0UL;
            if (((1UL << adc_map[i].index) & POWER_REQUIRED_VALID_MASK) != 0UL)
                sample.fault_bits |= POWER_FAULT_ADC_TIMEOUT;
            continue;
        }
        sample.raw[i] = raw;
        sample.pin_mv[i] = raw_to_pin_mv(raw);
        sample.valid_mask |= 1UL << i;
        ++sample.conversion_count;
        if (((1UL << adc_map[i].index) & POWER_REQUIRED_VALID_MASK) != 0UL &&
            raw >= 4090U) sample.fault_bits |= POWER_FAULT_ADC_RAIL;
        /* The CC6937 conditioner is unipolar and its calibrated zero is
         * near 8-10 mV (raw code about 10). Low raw current is therefore a
         * normal zero-load sample, not a sensor-disconnect fault. The
         * calibrated delta OCP check below is the current protection path. */
    }
    ++sample.sequence;
    if (fast_valid != 0UL) {
        uint32_t ff = fast.fault_flags;
        control.fast_sample_sequence = fast.sequence;
        control.fast_adc_irq_count = fast.irq_count;
        control.fast_adc_incomplete_count = fast.incomplete_count;
        control.fast_adc_fault_flags = ff;
        control.current_limit_active = fast.current_limit_active;
        control.per_cycle_sampling_running = fast_running;
        if ((ff & BOARD_FAST_ADC_FAULT_RAIL) != 0UL)
            control.fault_bits |= POWER_FAULT_ADC_RAIL;
        if ((ff & BOARD_FAST_ADC_FAULT_SEQUENCE) != 0UL)
            control.fault_bits |= POWER_FAULT_ADC_TIMEOUT;
        if ((ff & BOARD_FAST_ADC_FAULT_VBUS_OVP) != 0UL)
            control.fault_bits |= POWER_FAULT_VBUS_OVP;
        if ((ff & BOARD_FAST_ADC_FAULT_PV_OVP) != 0UL)
            control.fault_bits |= POWER_FAULT_PV_OVP;
        if ((ff & BOARD_FAST_ADC_FAULT_CURRENT_SENSOR) != 0UL)
            control.fault_bits |= POWER_FAULT_CURRENT_SENSOR;
        if ((ff & (BOARD_FAST_ADC_FAULT_I1_OCP |
                   BOARD_FAST_ADC_FAULT_I2_OCP)) != 0UL)
            control.fault_bits |= POWER_FAULT_OCP;
        if ((ff & BOARD_FAST_ADC_FAULT_I1_OCP) != 0UL)
            control.fault_bits |= POWER_FAULT_OCP_I1;
        if ((ff & BOARD_FAST_ADC_FAULT_I2_OCP) != 0UL)
            control.fault_bits |= POWER_FAULT_OCP_I2;
    } else {
        control.per_cycle_sampling_running = fast_running;
    }
    control.fault_bits |= sample.fault_bits &
        (POWER_FAULT_ADC_INIT | POWER_FAULT_ADC_TIMEOUT |
         POWER_FAULT_ADC_RAIL | POWER_FAULT_CURRENT_SENSOR);
    control.sample_sequence = sample.sequence;
    ++control.scan_count;
    control.ia_pin_mv = sample.pin_mv[POWER_ADC_I1];
    control.ib_pin_mv = sample.pin_mv[POWER_ADC_I2];
    control.pv_pin_mv = sample.pin_mv[POWER_ADC_VPV];
    control.vbus_pin_mv = sample.pin_mv[POWER_ADC_VBUS];
    control.pv_mv = ((sample.valid_mask & (1UL << POWER_ADC_VPV)) != 0UL) ?
        (int32_t)apply_gain_ppm(
            pv_pin_to_input_mv(sample.pin_mv[POWER_ADC_VPV]),
            control.voltage_calibration_valid ? pv_gain_ppm : 1000000UL) : 0;
    control.vbus_mv = ((sample.valid_mask & (1UL << POWER_ADC_VBUS)) != 0UL) ?
        (int32_t)apply_gain_ppm(
            pin_to_engineering_mv(sample.pin_mv[POWER_ADC_VBUS],
                                  VBUS_DIV_TOP_OHM, VBUS_DIV_BOTTOM_OHM),
            control.voltage_calibration_valid ? vbus_gain_ppm : 1000000UL) : 0;
    control.i1_est_ma = ((sample.valid_mask & (1UL << POWER_ADC_I1)) != 0UL) ?
                        (int32_t)(((uint32_t)sample.pin_mv[POWER_ADC_I1] * 1000UL +
                                   CURRENT_SENSOR_NOMINAL_MV_PER_A / 2UL) /
                                  CURRENT_SENSOR_NOMINAL_MV_PER_A) : 0;
    control.i2_est_ma = ((sample.valid_mask & (1UL << POWER_ADC_I2)) != 0UL) ?
                        (int32_t)(((uint32_t)sample.pin_mv[POWER_ADC_I2] * 1000UL +
                                   CURRENT_SENSOR_NOMINAL_MV_PER_A / 2UL) /
                                  CURRENT_SENSOR_NOMINAL_MV_PER_A) : 0;
    control.temperature_valid_mask = 0UL;
    control.ntc1_cdeg = 0;
    control.ntc2_cdeg = 0;
    if ((sample.valid_mask & (1UL << POWER_ADC_NTC1)) != 0UL &&
        ntc_raw_to_cdeg(sample.raw[POWER_ADC_NTC1], &control.ntc1_cdeg) != 0UL) {
        control.temperature_valid_mask |= 1UL << 0;
        ntc1_invalid_samples = 0UL;
        if (control.ntc1_cdeg >= BOARD_NTC_OTP_TRIP_CDEG) {
            if (ntc1_otp_samples < BOARD_OTP_CONFIRM_SAMPLES)
                ++ntc1_otp_samples;
        } else if (control.ntc1_cdeg <= BOARD_NTC_OTP_RECOVER_CDEG) {
            ntc1_otp_samples = 0UL;
        }
    } else {
        if (ntc1_invalid_samples < BOARD_NTC_INVALID_CONFIRM_SAMPLES)
            ++ntc1_invalid_samples;
    }
    if ((sample.valid_mask & (1UL << POWER_ADC_NTC2)) != 0UL &&
        ntc_raw_to_cdeg(sample.raw[POWER_ADC_NTC2], &control.ntc2_cdeg) != 0UL) {
        control.temperature_valid_mask |= 1UL << 1;
        ntc2_invalid_samples = 0UL;
        if (control.ntc2_cdeg >= BOARD_NTC_OTP_TRIP_CDEG) {
            if (ntc2_otp_samples < BOARD_OTP_CONFIRM_SAMPLES)
                ++ntc2_otp_samples;
        } else if (control.ntc2_cdeg <= BOARD_NTC_OTP_RECOVER_CDEG) {
            ntc2_otp_samples = 0UL;
        }
    } else {
        if (ntc2_invalid_samples < BOARD_NTC_INVALID_CONFIRM_SAMPLES)
            ++ntc2_invalid_samples;
    }
    control.ntc1_invalid_count = ntc1_invalid_samples;
    control.ntc2_invalid_count = ntc2_invalid_samples;
    if (ntc1_invalid_samples >= BOARD_NTC_INVALID_CONFIRM_SAMPLES ||
        ntc2_invalid_samples >= BOARD_NTC_INVALID_CONFIRM_SAMPLES)
        control.fault_bits |= POWER_FAULT_NTC_SENSOR;
    if (ntc1_otp_samples >= BOARD_OTP_CONFIRM_SAMPLES)
        control.fault_bits |= POWER_FAULT_OTP_NTC1;
    if (ntc2_otp_samples >= BOARD_OTP_CONFIRM_SAMPLES)
        control.fault_bits |= POWER_FAULT_OTP_NTC2;
    if (control.current_calibration_valid != 0UL &&
        control.current_polarity_valid != 0UL &&
        (sample.valid_mask & ((1UL << POWER_ADC_I1) | (1UL << POWER_ADC_I2))) ==
        ((1UL << POWER_ADC_I1) | (1UL << POWER_ADC_I2))) {
        int32_t delta_i1 = (int32_t)sample.pin_mv[POWER_ADC_I1] -
                           (int32_t)current_zero_i1_mv;
        int32_t delta_i2 = (int32_t)sample.pin_mv[POWER_ADC_I2] -
                           (int32_t)current_zero_i2_mv;
        control.i1_ma = current_polarity_i1 * delta_i1 * 1000 /
                        (int32_t)current_gain_i1_mv_per_a;
        control.i2_ma = current_polarity_i2 * delta_i2 * 1000 /
                        (int32_t)current_gain_i2_mv_per_a;
        control.pv_current_ma = saturate_i64_to_i32(
            (int64_t)control.i1_ma + (int64_t)control.i2_ma);
        control.pv_power_mw = saturate_i64_to_i32(
            ((int64_t)control.pv_mv * (int64_t)control.pv_current_ma) /
            1000LL);
        if (abs_i32_u32(control.i1_ma) >= CURRENT_OCP_LIMIT_MA ||
            abs_i32_u32(control.i2_ma) >= CURRENT_OCP_LIMIT_MA) {
            if (ocp_samples < OCP_CONFIRM_SAMPLES) ++ocp_samples;
        } else {
            ocp_samples = 0UL;
        }
        if (ocp_samples >= OCP_CONFIRM_SAMPLES) {
            control.fault_bits |= POWER_FAULT_OCP;
            if (abs_i32_u32(control.i1_ma) >= CURRENT_OCP_LIMIT_MA) {
                control.fault_bits |= POWER_FAULT_OCP_I1;
            }
            if (abs_i32_u32(control.i2_ma) >= CURRENT_OCP_LIMIT_MA) {
                control.fault_bits |= POWER_FAULT_OCP_I2;
            }
            sample.fault_bits |= POWER_FAULT_OCP;
        }
    } else {
        control.i1_ma = 0;
        control.i2_ma = 0;
        control.pv_current_ma = 0;
        control.pv_power_mw = 0;
        ocp_samples = 0UL;
    }
    if (filter_initialized == 0UL) {
        control.pv_filtered_mv = control.pv_mv;
        control.pv_current_filtered_ma = control.pv_current_ma;
        control.pv_power_filtered_mw = control.pv_power_mw;
        filter_initialized = 1UL;
    } else {
        control.pv_filtered_mv = iir_shift(control.pv_filtered_mv,
            control.pv_mv, BOARD_MPPT_FILTER_SHIFT);
        control.pv_current_filtered_ma = iir_shift(
            control.pv_current_filtered_ma, control.pv_current_ma,
            BOARD_MPPT_FILTER_SHIFT);
        control.pv_power_filtered_mw = iir_shift(
            control.pv_power_filtered_mw, control.pv_power_mw,
            BOARD_MPPT_FILTER_SHIFT);
    }
    if ((sample.valid_mask & (1UL << POWER_ADC_VBUS)) != 0UL) {
        if ((uint32_t)control.vbus_mv >= control.vbus_dynamic_ovp_limit_mv) {
            if (vbus_ovp_samples < OVP_CONFIRM_SAMPLES) ++vbus_ovp_samples;
        } else {
            vbus_ovp_samples = 0UL;
        }
        if (vbus_ovp_samples >= OVP_CONFIRM_SAMPLES) {
            control.fault_bits |= POWER_FAULT_VBUS_OVP;
            sample.fault_bits |= POWER_FAULT_VBUS_OVP;
        }
    }
    if ((sample.valid_mask & (1UL << POWER_ADC_VPV)) != 0UL) {
        if ((uint32_t)control.pv_mv >= PV_OVP_LIMIT_MV) {
            if (pv_ovp_samples < OVP_CONFIRM_SAMPLES) ++pv_ovp_samples;
        } else {
            pv_ovp_samples = 0UL;
        }
        if (pv_ovp_samples >= OVP_CONFIRM_SAMPLES) {
            control.fault_bits |= POWER_FAULT_PV_OVP;
            sample.fault_bits |= POWER_FAULT_PV_OVP;
        }
    }
    if (control.mode == POWER_CONTROL_OFF && control.outputs_enabled == 0UL) {
        /* These bits describe a running converter, not an unpowered sensor
         * snapshot.  Clear stale startup/transient indications so a later
         * AUX or MPPT command is not rejected by an old SAFE_OFF sample. */
        control.fault_bits &= ~(POWER_FAULT_ADC_TIMEOUT |
                                POWER_FAULT_CURRENT_SENSOR |
                                POWER_FAULT_NTC_SENSOR |
                                POWER_FAULT_BOOST_UNREACHABLE |
                                POWER_FAULT_PV_UVLO |
                                POWER_FAULT_CURRENT_IMBALANCE |
                                POWER_FAULT_CURRENT_STUCK |
                                POWER_FAULT_VBUS_BUILD_TIMEOUT |
                                POWER_FAULT_DUTY_SATURATION |
                                POWER_FAULT_POWER_LIMIT |
                                POWER_FAULT_SAMPLE_STALE);
    }
    update_runtime_supervision();
    contain_critical_faults();
    return ok;
}

static uint32_t clamp_duty(int32_t duty)
{
    if (duty < (int32_t)CONTROL_DUTY_MIN_Q15) return CONTROL_DUTY_MIN_Q15;
    if (duty > (int32_t)CONTROL_DUTY_MAX_Q15) return CONTROL_DUTY_MAX_Q15;
    return (uint32_t)duty;
}

static void control_fast_step(void)
{
    if (BOARD_POWER_OUTPUT_ARMING_ENABLE == 0U ||
        control.mode == POWER_CONTROL_OFF || control.calibration_valid == 0UL ||
        control.hrtim_backend_ready == 0UL || control.protection_backend_ready == 0UL ||
        external_fault_latched != 0UL) {
        control.duty_q15 = 0UL;
        control.outputs_enabled = 0UL;
        control.softstart_limit_q15 = 0UL;
        control.compare_write_ok = 0UL;
        board_fast_adc_set_duty_limit_command(0UL, 0U, 0U, 0UL);
        return;
    }
    if (control.softstart_limit_q15 < CONTROL_DUTY_MIN_Q15)
        control.softstart_limit_q15 = CONTROL_DUTY_MIN_Q15;
    else if (control.softstart_limit_q15 < CONTROL_DUTY_MAX_Q15) {
        uint32_t next = control.softstart_limit_q15 + BOARD_SOFTSTART_STEP_Q15;
        control.softstart_limit_q15 = (next > CONTROL_DUTY_MAX_Q15) ?
                                      CONTROL_DUTY_MAX_Q15 : next;
    }
    control.duty_q15 = clamp_duty((int32_t)control.proposed_duty_q15);
    if (control.duty_q15 > control.softstart_limit_q15)
        control.duty_q15 = control.softstart_limit_q15;
    board_fast_adc_set_duty_limit_command(control.duty_q15,
        pin_mv_to_raw((current_gain_i1_mv_per_a * CURRENT_OPERATING_LIMIT_MA +
                       500UL) / 1000UL),
        pin_mv_to_raw((current_gain_i2_mv_per_a * CURRENT_OPERATING_LIMIT_MA +
                       500UL) / 1000UL),
        control.current_calibration_valid);
    control.compare_write_ok = board_hrtim_set_duty_q15(control.duty_q15);
    if (control.compare_write_ok != 0UL &&
        board_hrtim_power_outputs_enabled() == 0UL)
        (void)board_safety_request_power_on();
    control.outputs_enabled = board_hrtim_power_outputs_enabled();
}

static void control_outer_step(void)
{
    int32_t error;
    int32_t proportional;
    uint32_t new_limit_active;
    int64_t candidate;
    if (control.mode == POWER_CONTROL_OFF || control.calibration_valid == 0UL ||
        (sample.valid_mask & POWER_REQUIRED_VALID_MASK) !=
        POWER_REQUIRED_VALID_MASK) {
        control.proposed_duty_q15 = 0UL;
        return;
    }
    new_limit_active = control.vbus_limit_active;
    if (control.mode != POWER_CONTROL_MPPT) new_limit_active = 1UL;
    else if (control.vbus_mv >= (int32_t)control.target_mv)
        new_limit_active = 1UL;
    else if (control.vbus_mv <=
             (int32_t)(control.target_mv -
                       ((control.target_mv > VBUS_LIMIT_RELEASE_HYST_MV) ?
                        VBUS_LIMIT_RELEASE_HYST_MV : 0UL)))
        new_limit_active = 0UL;
    if (control.mode == POWER_CONTROL_MPPT && new_limit_active == 0UL) {
        /* Build phase: VBUS is below target, so regulate the bus first.  The
         * previous implementation used PV error here; because pv_reference
         * starts at the measured PV voltage, that produced a zero-duty dead
         * start and eventually a build-timeout shutdown. */
        error = (int32_t)control.target_mv - control.vbus_mv;
    } else if (control.mode == POWER_CONTROL_MPPT) {
        /* At the bus target, use the PV reference for MPPT loading while the
         * bus limit remains active. */
        error = control.pv_filtered_mv - (int32_t)control.pv_reference_mv;
    } else {
        error = (int32_t)control.target_mv - control.vbus_mv;
    }
    proportional = (int32_t)(((int64_t)error *
                    BOARD_OUTER_KP_Q15_PER_MV_NUM) /
                    BOARD_OUTER_KP_Q15_PER_MV_DEN);
    if (new_limit_active != control.vbus_limit_active) {
        /* Back-calculate the integrator so changing controlled variable does
         * not create an immediate duty step. */
        outer_integrator_q15 = (int32_t)control.proposed_duty_q15 - proportional;
    }
    control.vbus_limit_active = new_limit_active;
    candidate = (int64_t)outer_integrator_q15 +
                ((int64_t)error * BOARD_OUTER_KI_Q15_PER_MV_NUM) /
                BOARD_OUTER_KI_Q15_PER_MV_DEN;
    if (candidate < 0) candidate = 0;
    if (candidate > (int64_t)CONTROL_DUTY_MAX_Q15)
        candidate = CONTROL_DUTY_MAX_Q15;
    outer_integrator_q15 = (int32_t)candidate;
    control.proposed_duty_q15 = clamp_duty(saturate_i64_to_i32(
        (int64_t)proportional + candidate));
}

static void control_mppt_step(void)
{
    int32_t delta;
    int32_t candidate;
    int32_t step;
    uint32_t deadband;
    if (control.mode != POWER_CONTROL_MPPT || control.calibration_valid == 0UL) return;
    if (control.sample_sequence == previous_mppt_sample_sequence ||
        control.pv_power_filtered_mw <= 0 ||
        control.pv_current_filtered_ma <= 0) {
        ++control.mppt_invalid_sample_count;
        return;
    }
    previous_mppt_sample_sequence = control.sample_sequence;
    delta = control.pv_power_filtered_mw - previous_power_mw;
    deadband = (uint32_t)(((uint64_t)abs_i32_u32(previous_power_mw) *
                           BOARD_MPPT_RELATIVE_DEADBAND_PPM + 500000ULL) /
                          1000000ULL);
    if (deadband < MPPT_POWER_DEADBAND_MIN_MW)
        deadband = MPPT_POWER_DEADBAND_MIN_MW;
    if (abs_i32_u32(delta) > deadband && delta < 0)
        mppt_direction = -mppt_direction;
    step = (abs_i32_u32(delta) > (deadband * 4UL)) ?
           (int32_t)BOARD_MPPT_STEP_MAX_MV :
           (int32_t)BOARD_MPPT_STEP_MIN_MV;
    candidate = (int32_t)control.pv_reference_mv + mppt_direction * step;
    if (candidate < 20000) candidate = 20000;
    if (candidate > (int32_t)BOARD_PV_OPERATING_MAX_MV)
        candidate = (int32_t)BOARD_PV_OPERATING_MAX_MV;
    control.pv_reference_mv = (uint32_t)candidate;
    previous_power_mw = control.pv_power_filtered_mw;
    ++control.mppt_perturb_count;
}

void power_control_init(void)
{
    uint32_t ok;
    uint32_t i;
    for (i = 0UL; i < POWER_ADC_CHANNEL_COUNT; ++i) {
        sample.raw[i] = 0U; sample.pin_mv[i] = 0U;
    }
    sample.sequence = 0UL; sample.valid_mask = 0UL; sample.fault_bits = 0UL;
    sample.conversion_count = 0UL;
    control.mode = POWER_CONTROL_OFF;
    control.target_mv = CONTROL_DEFAULT_TARGET_MV;
    control.pv_reference_mv = 0UL;
    control.duty_q15 = 0UL; control.proposed_duty_q15 = 0UL;
    control.sample_sequence = 0UL; control.scan_count = 0UL;
    control.fast_sample_sequence = 0UL;
    control.fast_adc_irq_count = 0UL;
    control.fast_adc_incomplete_count = 0UL;
    control.fast_adc_fault_flags = 0UL;
    control.per_cycle_sampling_running = 0UL;
    control.fast_sample_rate_hz = 0UL;
    control.calibration_valid = 0UL;
    control.current_calibration_valid = 0UL;
    control.current_polarity_valid = 0UL;
    control.voltage_calibration_valid = 0UL;
    control.v15_sense_valid = 0UL;
    control.hrtim_backend_ready = board_hrtim_backend_ready();
    control.protection_backend_ready = board_safety_protection_ready();
    control.external_fault_latched = 0UL;
    control.outputs_enabled = 0UL;
    control.fault_bits = POWER_FAULT_CALIBRATION_REQUIRED;
    last_stop_fault_bits = 0UL;
    last_stop_fast_fault_flags = 0UL;
    last_stop_duty_q15 = 0UL;
    last_stop_time_ms = 0UL;
    control.last_stop_fault_bits = 0UL;
    control.last_stop_fast_fault_flags = 0UL;
    control.last_stop_duty_q15 = 0UL;
    control.last_stop_time_ms = 0UL;
    if (control.hrtim_backend_ready == 0UL)
        control.fault_bits |= POWER_FAULT_HRTIM_LOCK;
    if (control.protection_backend_ready == 0UL)
        control.fault_bits |= POWER_FAULT_PROTECTION_UNVERIFIED;
    if (control.v15_sense_valid == 0UL)
        control.fault_bits |= POWER_FAULT_V15_SENSE_UNAVAILABLE;
    ok = adc_backend_init();
    adc_ready_mask = ok;
    control.adc_backend_ready = ((ok & 0x7UL) == 0x7UL) ? 1UL : 0UL;
    control.adc_ready_mask = ok;
    if ((ok & 0x7UL) != 0x7UL) control.fault_bits |= POWER_FAULT_ADC_INIT;
    control.vbus_ovp_limit_mv = VBUS_OVP_LIMIT_MV;
    control.vbus_dynamic_ovp_limit_mv = dynamic_vbus_ovp_limit_mv();
    control.vbus_ovp_raw_threshold =
        vbus_input_mv_to_raw(control.vbus_dynamic_ovp_limit_mv);
    control.pv_ovp_limit_mv = PV_OVP_LIMIT_MV;
    control.ocp_limit_ma = CURRENT_OCP_LIMIT_MA;
    control.uvlo_15v_limit_mv = AUX_UVLO_LIMIT_MV;
    control.ntc_otp_trip_cdeg = BOARD_NTC_OTP_TRIP_CDEG;
    control.ntc_otp_recover_cdeg = BOARD_NTC_OTP_RECOVER_CDEG;
    control.controller_gains_validated = BOARD_CONTROL_GAINS_VALIDATED;
    control.softstart_limit_q15 = 0UL;
    control.vbus_limit_active = 0UL;
    control.boost_reachable = 0UL;
    control.compare_write_ok = 0UL;
    vbus_ovp_samples = 0UL;
    pv_ovp_samples = 0UL;
    schedule_started = 0UL;
    next_scan = 0UL; next_fast = 0UL; next_outer = 0UL; next_mppt = 0UL;
    next_fast_rate = 0UL; previous_fast_sequence = 0UL;
    previous_fast_rate_ms = 0UL;
    watched_fast_sequence = 0UL;
    last_fast_progress_ms = 0UL;
    outer_integrator_q15 = 0; previous_power_mw = 0; mppt_direction = -1;
    filter_initialized = 0UL;
    ocp_samples = 0UL; current_zero_i1_mv = 0UL; current_zero_i2_mv = 0UL;
    current_gain_i1_mv_per_a = 0UL;
    current_gain_i2_mv_per_a = 0UL;
    current_polarity_i1 = 0;
    current_polarity_i2 = 0;
    ntc1_invalid_samples = 0UL; ntc2_invalid_samples = 0UL;
    ntc1_otp_samples = 0UL; ntc2_otp_samples = 0UL;
    pv_gain_ppm = 0UL;
    vbus_gain_ppm = 0UL;
    external_fault_latched = 0UL;
    last_poll_ms = 0UL; mode_start_ms = 0UL;
    current_imbalance_ms = 0UL; current_stuck_ms = 0UL;
    duty_saturation_ms = 0UL; power_limit_ms = 0UL;
    previous_mppt_sample_sequence = 0UL;
    control.current_zero_i1_mv = 0UL;
    control.current_zero_i2_mv = 0UL;
    control.current_gain_i1_mv_per_a = 0UL;
    control.current_gain_i2_mv_per_a = 0UL;
    control.current_polarity_i1 = 0;
    control.current_polarity_i2 = 0;
    control.pv_gain_ppm = 0UL;
    control.vbus_gain_ppm = 0UL;
    control.i1_ma = 0;
    control.i2_ma = 0;
    control.i1_est_ma = 0;
    control.i2_est_ma = 0;
    control.pv_filtered_mv = 0;
    control.pv_current_filtered_ma = 0;
    control.pv_power_filtered_mw = 0;
    control.ntc1_cdeg = 0;
    control.ntc2_cdeg = 0;
    control.temperature_valid_mask = 0UL;
    update_io_status();
    update_fast_adc_limits();
#if BOARD_PER_CYCLE_ADC_ENABLE
    if (board_fast_adc_init() == 0UL) {
        control.fault_bits |= POWER_FAULT_ADC_INIT | POWER_FAULT_HRTIM_LOCK;
        control.per_cycle_sampling_running = 0UL;
    } else {
        resume_fast_sampling();
    }
#endif
    contain_critical_faults();
}

uint32_t power_control_scan_now(void)
{
    if (control.adc_backend_ready == 0UL) return 0UL;
    return adc_scan();
}

void power_control_poll_ms(uint32_t now_ms)
{
    last_poll_ms = now_ms;
    if (schedule_started == 0UL) {
        schedule_started = 1UL;
        next_scan = now_ms + CONTROL_SCAN_PERIOD_MS;
        next_fast = now_ms + CONTROL_FAST_PERIOD_MS;
        next_outer = now_ms + CONTROL_OUTER_PERIOD_MS;
        next_mppt = now_ms + CONTROL_MPPT_PERIOD_MS;
        next_fast_rate = now_ms + 1000UL;
        previous_fast_sequence = control.fast_sample_sequence;
        previous_fast_rate_ms = now_ms;
        watched_fast_sequence = control.fast_sample_sequence;
        last_fast_progress_ms = now_ms;
        return;
    }
    if (board_hrtim_sampling_is_running() != 0UL) {
        board_fast_adc_snapshot_t watched;
        if (board_fast_adc_get_snapshot(&watched) != 0UL &&
            watched.sequence != watched_fast_sequence) {
            watched_fast_sequence = watched.sequence;
            last_fast_progress_ms = now_ms;
        } else if ((uint32_t)(now_ms - last_fast_progress_ms) >=
                   FAST_ADC_STALL_TIMEOUT_MS) {
            board_fast_adc_trip_sequence_timeout();
            control.fast_adc_fault_flags |= BOARD_FAST_ADC_FAULT_SEQUENCE;
            ++control.fast_adc_incomplete_count;
            control.per_cycle_sampling_running = 0UL;
            control.fault_bits |= POWER_FAULT_ADC_TIMEOUT;
            control.fault_bits |= POWER_FAULT_SAMPLE_STALE;
            contain_critical_faults();
        }
    } else {
        last_fast_progress_ms = now_ms;
    }
    if ((int32_t)(now_ms - next_scan) >= 0) {
        next_scan += CONTROL_SCAN_PERIOD_MS;
        (void)adc_scan();
    }
    enforce_boost_reachability();
    if ((int32_t)(now_ms - next_fast) >= 0) {
        next_fast += CONTROL_FAST_PERIOD_MS;
        control_fast_step();
    }
    if ((int32_t)(now_ms - next_outer) >= 0) {
        next_outer += CONTROL_OUTER_PERIOD_MS;
        control_outer_step();
    }
    if ((int32_t)(now_ms - next_mppt) >= 0) {
        next_mppt += CONTROL_MPPT_PERIOD_MS;
        control_mppt_step();
    }
    if ((int32_t)(now_ms - next_fast_rate) >= 0) {
        board_fast_adc_snapshot_t fast;
        uint32_t elapsed_ms = now_ms - previous_fast_rate_ms;
        next_fast_rate += 1000UL;
        if (board_fast_adc_get_snapshot(&fast) != 0UL) {
            uint32_t delta = fast.sequence - previous_fast_sequence;
            control.fast_sample_rate_hz = (elapsed_ms != 0UL) ?
                (uint32_t)(((uint64_t)delta * 1000ULL + elapsed_ms / 2UL) /
                           elapsed_ms) : 0UL;
            previous_fast_sequence = fast.sequence;
        } else {
            control.fast_sample_rate_hz = 0UL;
        }
        previous_fast_rate_ms = now_ms;
    }
    contain_critical_faults();
}

void power_control_get_sample(power_sample_t *out)
{
    uint32_t i;
    if (out == (power_sample_t *)0) return;
    *out = sample;
    for (i = 0UL; i < POWER_ADC_CHANNEL_COUNT; ++i) {
        out->raw[i] = sample.raw[i]; out->pin_mv[i] = sample.pin_mv[i];
    }
}

void power_control_get_status(power_control_status_t *out)
{
    if (out != (power_control_status_t *)0) *out = control;
}

void power_control_set_external_fault_latched(uint32_t latched)
{
    external_fault_latched = (latched != 0UL) ? 1UL : 0UL;
    control.external_fault_latched = external_fault_latched;
    if (external_fault_latched != 0UL) {
        control.mode = POWER_CONTROL_OFF;
        control.duty_q15 = 0UL;
        control.proposed_duty_q15 = 0UL;
        control.outputs_enabled = 0UL;
        board_safety_force_off();
    }
}

int power_control_set_mode(power_control_mode_t mode)
{
    uint32_t blocking_faults;
    if (mode > POWER_CONTROL_MPPT) return -1;
    if (mode == POWER_CONTROL_OFF) {
        control.mode = POWER_CONTROL_OFF; control.duty_q15 = 0UL;
        control.proposed_duty_q15 = 0UL; control.outputs_enabled = 0UL;
        outer_integrator_q15 = 0;
        control.softstart_limit_q15 = 0UL;
        control.vbus_limit_active = 0UL;
        control.boost_reachable = 0UL;
        control.fault_bits &= ~POWER_FAULT_BOOST_UNREACHABLE;
        board_safety_force_off();
        return 0;
    }
    if (external_fault_latched != 0UL) return -4;
    if (control.pv_mv < (int32_t)BOARD_PV_UVLO_START_MV) {
        control.fault_bits |= POWER_FAULT_PV_UVLO;
        return -5;
    }
    control.fault_bits &= ~POWER_FAULT_PV_UVLO;
    control.boost_reachable = boost_input_is_reachable();
    if (control.boost_reachable == 0UL) {
        control.fault_bits |= POWER_FAULT_BOOST_UNREACHABLE;
        return -3;
    }
    control.fault_bits &= ~POWER_FAULT_BOOST_UNREACHABLE;
    /* HRTIM_LOCK is a recoverable runtime status while outputs are off.  The
     * mode transition below restarts the ADC/HRTIM sampling epoch and its
     * result is the authoritative check.  Treating a stale lock bit as a
     * precondition here made a failed/aborted START permanently self-locking. */
    blocking_faults = control.fault_bits & POWER_CRITICAL_FAULT_MASK;
    blocking_faults &= ~POWER_FAULT_HRTIM_LOCK;
    if (BOARD_POWER_OUTPUT_ARMING_ENABLE == 0U ||
#if !BOARD_EXTERNAL_PROTECTION_CONFIRMED
        BOARD_CONTROL_GAINS_VALIDATED == 0U ||
#endif
        control.calibration_valid == 0UL || control.hrtim_backend_ready == 0UL ||
        control.protection_backend_ready == 0UL ||
        external_fault_latched != 0UL ||
        blocking_faults != 0UL ||
        board_safety_power_outputs_are_off() == 0UL) return -2;
    if (mode == POWER_CONTROL_MPPT &&
        (sample.valid_mask & (1UL << POWER_ADC_VPV)) != 0UL) {
        int32_t reference = control.pv_mv;
        if (reference < 20000) reference = 20000;
        if (reference > (int32_t)BOARD_PV_OPERATING_MAX_MV)
            reference = (int32_t)BOARD_PV_OPERATING_MAX_MV;
        control.pv_reference_mv = (uint32_t)reference;
        previous_power_mw = control.pv_power_filtered_mw;
        mppt_direction = -1;
        previous_mppt_sample_sequence = control.sample_sequence;
    }
    control.softstart_limit_q15 = 0UL;
    control.vbus_limit_active = (mode == POWER_CONTROL_CV) ? 1UL : 0UL;
    control.mode = mode;
    /* The command path deliberately forces every output and the injected ADC
     * trigger off before changing mode.  Restart the PWM-synchronous sampling
     * epoch here; otherwise the next poll sees HRTIM sampling enabled but a
     * frozen ADC sequence and trips the 5-ms stale-sample guard before the
     * first duty update can be applied. */
    resume_fast_sampling();
    if (control.per_cycle_sampling_running == 0UL) {
        control.mode = POWER_CONTROL_OFF;
        control.duty_q15 = 0UL;
        control.proposed_duty_q15 = 0UL;
        control.outputs_enabled = 0UL;
        control.fault_bits |= POWER_FAULT_HRTIM_LOCK;
        board_safety_force_power_off();
        return -6;
    }
    /* Seed a non-zero command for the first control tick. The outer loop now
     * uses VBUS error during the build phase, so the converter cannot remain
     * at zero duty while waiting for an MPPT perturbation. */
    outer_integrator_q15 = (int32_t)CONTROL_DUTY_MIN_Q15;
    control.proposed_duty_q15 = CONTROL_DUTY_MIN_Q15;
    control.softstart_limit_q15 = CONTROL_DUTY_MIN_Q15;
    mode_start_ms = last_poll_ms;
    current_imbalance_ms = 0UL; current_stuck_ms = 0UL;
    boost_unreachable_ms = 0UL;
    duty_saturation_ms = 0UL; power_limit_ms = 0UL;
    /* START is successful only after the compare write, HRTIM counters, PWM
     * pins, OE3, PA2 inhibit and PA5 request have all been applied.  This
     * prevents the command layer from reporting OK while the physical output
     * path is still locked. */
    control_outer_step();
    control_fast_step();
    if (control.compare_write_ok == 0UL || control.outputs_enabled == 0UL) {
        control.fault_bits |= POWER_FAULT_HRTIM_LOCK;
        contain_critical_faults();
        return -7;
    }
    return 0;
}

int power_control_set_target_mv(uint32_t target_mv)
{
    if (target_mv == 0UL || target_mv > BOARD_CONTROL_TARGET_MAX_MV) return -1;
    control.target_mv = target_mv;
    control.vbus_dynamic_ovp_limit_mv = dynamic_vbus_ovp_limit_mv();
    update_fast_adc_limits();
    enforce_boost_reachability();
    return 0;
}

int power_control_set_current_calibration(uint32_t zero_i1_mv,
                                          uint32_t zero_i2_mv,
                                          uint32_t gain_i1_mv_per_a,
                                          uint32_t gain_i2_mv_per_a)
{
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    board_safety_force_off();
    /* The CC6937 conditioner on this board is a unipolar, near-ground
     * current output at zero load (the measured idle value is about 8-10 mV).
     * The previous lower-bound/headroom check incorrectly rejected that real
     * zero point by requiring zero > OCP_delta + 20 mV.  Only the upper rail
     * headroom is required for the signed software estimate used below. */
    if (zero_i1_mv > (ADC_FULL_SCALE_MV - 20UL) ||
        zero_i2_mv > (ADC_FULL_SCALE_MV - 20UL) ||
        gain_i1_mv_per_a < BOARD_CURRENT_GAIN_MIN_MV_PER_A ||
        gain_i1_mv_per_a > BOARD_CURRENT_GAIN_MAX_MV_PER_A ||
        gain_i2_mv_per_a < BOARD_CURRENT_GAIN_MIN_MV_PER_A ||
        gain_i2_mv_per_a > BOARD_CURRENT_GAIN_MAX_MV_PER_A ||
        zero_i1_mv + (CURRENT_OCP_LIMIT_MA * gain_i1_mv_per_a) / 1000UL >=
            ADC_FULL_SCALE_MV - 20UL ||
        zero_i2_mv + (CURRENT_OCP_LIMIT_MA * gain_i2_mv_per_a) / 1000UL >=
            ADC_FULL_SCALE_MV - 20UL) return -1;
    current_zero_i1_mv = zero_i1_mv;
    current_zero_i2_mv = zero_i2_mv;
    current_gain_i1_mv_per_a = gain_i1_mv_per_a;
    current_gain_i2_mv_per_a = gain_i2_mv_per_a;
    control.current_calibration_valid = 1UL;
    control.current_zero_i1_mv = zero_i1_mv;
    control.current_zero_i2_mv = zero_i2_mv;
    control.current_gain_i1_mv_per_a = gain_i1_mv_per_a;
    control.current_gain_i2_mv_per_a = gain_i2_mv_per_a;
    update_fast_adc_limits();
    update_calibration_state();
    resume_fast_sampling();
    return 0;
}

int power_control_set_current_polarity(int32_t polarity_i1,
                                       int32_t polarity_i2)
{
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    board_safety_force_off();
    if ((polarity_i1 != 1 && polarity_i1 != -1) ||
        (polarity_i2 != 1 && polarity_i2 != -1)) return -1;
    current_polarity_i1 = polarity_i1;
    current_polarity_i2 = polarity_i2;
    control.current_polarity_i1 = polarity_i1;
    control.current_polarity_i2 = polarity_i2;
    control.current_polarity_valid = 1UL;
    update_calibration_state();
    return 0;
}

int power_control_set_voltage_calibration(uint32_t pv_gain,
                                          uint32_t vbus_gain)
{
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    board_safety_force_off();
    if (pv_gain < BOARD_VOLTAGE_GAIN_MIN_PPM ||
        pv_gain > BOARD_VOLTAGE_GAIN_MAX_PPM ||
        vbus_gain < BOARD_VOLTAGE_GAIN_MIN_PPM ||
        vbus_gain > BOARD_VOLTAGE_GAIN_MAX_PPM) return -1;
    pv_gain_ppm = pv_gain;
    vbus_gain_ppm = vbus_gain;
    control.pv_gain_ppm = pv_gain;
    control.vbus_gain_ppm = vbus_gain;
    control.voltage_calibration_valid = 1UL;
    update_calibration_state();
    update_fast_adc_limits();
    resume_fast_sampling();
    return 0;
}

int power_control_auto_zero_current(uint32_t sample_count,
                                    uint32_t *zero_i1_mv,
                                    uint32_t *zero_i2_mv)
{
    uint64_t sum_i1 = 0ULL, sum_i2 = 0ULL;
    uint32_t i;
    int result;
    power_sample_t captured;

    if (sample_count < 8UL || sample_count > 256UL ||
        zero_i1_mv == (uint32_t *)0 || zero_i2_mv == (uint32_t *)0) return -1;
    board_safety_force_off();
    if (control.mode != POWER_CONTROL_OFF || control.outputs_enabled != 0UL ||
        board_safety_outputs_are_off() == 0UL) return -2;
    board_fast_adc_stop();
    for (i = 0UL; i < sample_count; ++i)
    {
        /* 64 ADC scans can exceed the nominal 2 s IWDG window on a slow
         * conversion corner; keep the operation bounded but service the
         * application watchdog between samples. */
        power_control_watchdog_kick();
        if (power_control_scan_now() == 0UL) { resume_fast_sampling(); return -3; }
        power_control_get_sample(&captured);
        if ((captured.valid_mask & ((1UL << POWER_ADC_I1) | (1UL << POWER_ADC_I2))) !=
            ((1UL << POWER_ADC_I1) | (1UL << POWER_ADC_I2))) {
            resume_fast_sampling(); return -4;
        }
        sum_i1 += captured.pin_mv[POWER_ADC_I1];
        sum_i2 += captured.pin_mv[POWER_ADC_I2];
    }
    *zero_i1_mv = (uint32_t)((sum_i1 + sample_count / 2UL) / sample_count);
    *zero_i2_mv = (uint32_t)((sum_i2 + sample_count / 2UL) / sample_count);
    result = power_control_set_current_calibration(*zero_i1_mv, *zero_i2_mv,
        CURRENT_SENSOR_NOMINAL_MV_PER_A, CURRENT_SENSOR_NOMINAL_MV_PER_A);
    resume_fast_sampling();
    return result;
}

void power_control_clear_current_calibration(void)
{
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    board_safety_force_off();
    current_zero_i1_mv = 0UL;
    current_zero_i2_mv = 0UL;
    current_gain_i1_mv_per_a = 0UL;
    current_gain_i2_mv_per_a = 0UL;
    current_polarity_i1 = 0;
    current_polarity_i2 = 0;
    control.current_calibration_valid = 0UL;
    control.current_polarity_valid = 0UL;
    control.current_zero_i1_mv = 0UL;
    control.current_zero_i2_mv = 0UL;
    control.current_gain_i1_mv_per_a = 0UL;
    control.current_gain_i2_mv_per_a = 0UL;
    control.current_polarity_i1 = 0;
    control.current_polarity_i2 = 0;
    update_fast_adc_limits();
    control.pv_current_ma = 0;
    control.pv_power_mw = 0;
    control.i1_ma = 0;
    control.i2_ma = 0;
    ocp_samples = 0UL;
    update_calibration_state();
    resume_fast_sampling();
}

void power_control_clear_voltage_calibration(void)
{
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    board_safety_force_off();
    pv_gain_ppm = 0UL;
    vbus_gain_ppm = 0UL;
    control.pv_gain_ppm = 0UL;
    control.vbus_gain_ppm = 0UL;
    control.voltage_calibration_valid = 0UL;
    update_calibration_state();
    update_fast_adc_limits();
    resume_fast_sampling();
}

void power_control_clear_all_calibration(void)
{
    power_control_clear_current_calibration();
    power_control_clear_voltage_calibration();
}

int power_control_export_calibration(power_calibration_t *calibration)
{
    if (calibration == (power_calibration_t *)0 ||
        control.calibration_valid == 0UL) return -1;
    calibration->schema = POWER_CALIBRATION_SCHEMA;
    calibration->zero_i1_mv = current_zero_i1_mv;
    calibration->zero_i2_mv = current_zero_i2_mv;
    calibration->gain_i1_mv_per_a = current_gain_i1_mv_per_a;
    calibration->gain_i2_mv_per_a = current_gain_i2_mv_per_a;
    calibration->polarity_i1 = current_polarity_i1;
    calibration->polarity_i2 = current_polarity_i2;
    calibration->pv_gain_ppm = pv_gain_ppm;
    calibration->vbus_gain_ppm = vbus_gain_ppm;
    return 0;
}

int power_control_import_calibration(const power_calibration_t *calibration)
{
    if (calibration == (const power_calibration_t *)0 ||
        calibration->schema != POWER_CALIBRATION_SCHEMA) return -1;
    if (power_control_set_current_calibration(calibration->zero_i1_mv,
            calibration->zero_i2_mv, calibration->gain_i1_mv_per_a,
            calibration->gain_i2_mv_per_a) != 0) return -2;
    if (power_control_set_current_polarity(calibration->polarity_i1,
            calibration->polarity_i2) != 0) return -3;
    if (power_control_set_voltage_calibration(calibration->pv_gain_ppm,
            calibration->vbus_gain_ppm) != 0) return -4;
    return 0;
}

int power_control_clear_faults(void)
{
    board_safety_force_off();
    if (control.mode != POWER_CONTROL_OFF) return -1;
    if ((sample.valid_mask & POWER_REQUIRED_VALID_MASK) !=
        POWER_REQUIRED_VALID_MASK ||
        sample.raw[POWER_ADC_I1] <= CURRENT_SENSOR_LOW_RAW ||
        sample.raw[POWER_ADC_I2] <= CURRENT_SENSOR_LOW_RAW ||
        sample.raw[POWER_ADC_I1] >= 4090U ||
        sample.raw[POWER_ADC_I2] >= 4090U) return -2;
    if (control.temperature_valid_mask != 3UL ||
        control.ntc1_cdeg > BOARD_NTC_OTP_RECOVER_CDEG ||
        control.ntc2_cdeg > BOARD_NTC_OTP_RECOVER_CDEG) return -3;
    if ((uint32_t)control.vbus_mv >= control.vbus_dynamic_ovp_limit_mv ||
        (uint32_t)control.pv_mv >= PV_OVP_LIMIT_MV) return -4;
    board_fast_adc_clear_fault_flags();
    control.fault_bits &= POWER_FAULT_ADC_INIT | POWER_FAULT_CALIBRATION_REQUIRED |
                          POWER_FAULT_HRTIM_LOCK | POWER_FAULT_PROTECTION_UNVERIFIED |
                          POWER_FAULT_V15_SENSE_UNAVAILABLE;
    resume_fast_sampling();
    control.i1_ma = 0;
    control.i2_ma = 0;
    control.pv_current_ma = 0;
    control.pv_power_mw = 0;
    vbus_ovp_samples = 0UL;
    pv_ovp_samples = 0UL;
    ocp_samples = 0UL;
    ntc1_invalid_samples = 0UL; ntc2_invalid_samples = 0UL;
    ntc1_otp_samples = 0UL; ntc2_otp_samples = 0UL;
    current_imbalance_ms = 0UL; current_stuck_ms = 0UL;
    boost_unreachable_ms = 0UL;
    duty_saturation_ms = 0UL; power_limit_ms = 0UL;
    last_stop_fault_bits = 0UL;
    last_stop_fast_fault_flags = 0UL;
    last_stop_duty_q15 = 0UL;
    last_stop_time_ms = 0UL;
    control.last_stop_fault_bits = 0UL;
    control.last_stop_fast_fault_flags = 0UL;
    control.last_stop_duty_q15 = 0UL;
    control.last_stop_time_ms = 0UL;
    return 0;
}

const char *power_control_mode_name(power_control_mode_t mode)
{
    if (mode == POWER_CONTROL_CV) return "CV";
    if (mode == POWER_CONTROL_MPPT) return "MPPT";
    return "OFF";
}
