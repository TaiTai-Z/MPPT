#include "board_fast_adc.h"
#include "board_config.h"
#include "board_hrtim.h"
#include "board_safety.h"
#include "stm32g474_bare.h"

/* Implemented by the application when the IWDG is enabled.  Keeping this as
 * a callback lets bounded peripheral waits remain diagnosable without making
 * the ADC driver depend on the RT-Thread API. */
extern void power_control_watchdog_kick(void);

/* STM32G4 injected-trigger encoding: these CMSIS JEXTSEL_* definitions are
 * already positioned at JSQR bits [6:2].  Do not shift them a second time.
 * The combination is ST's LL_ADC_INJ_TRIG_EXT_HRTIM_TRG2 (selector 19). */
#define FAST_ADC_JEXTSEL_HRTIM_TRG2 \
    (ADC_JSQR_JEXTSEL_4 | ADC_JSQR_JEXTSEL_1 | ADC_JSQR_JEXTSEL_0)
#define FAST_ADC_SAMPLE_CODE_47P5   4UL
#define FAST_ADC_RAIL_RAW           4090U
#define FAST_ADC_CURRENT_LOW_RAW    8U
#define FAST_ADC_FIRST_CYCLE_TIMEOUT_LOOPS 1000000UL
#define FAST_ADC_STOP_TIMEOUT_LOOPS         100000UL

static volatile board_fast_adc_snapshot_t fast_snapshot;
static volatile uint32_t snapshot_lock;
static volatile uint32_t stage_mask;
static volatile uint16_t stage_i1, stage_i2, stage_ntc1, stage_ntc2;
static volatile uint16_t stage_vpv, stage_vbus;
static volatile uint16_t limit_i1_zero, limit_i2_zero;
static volatile uint16_t limit_i1_delta, limit_i2_delta;
static volatile uint16_t limit_pv, limit_vbus;
static volatile uint32_t limit_current_valid;
static volatile uint32_t duty_command_q15;
static volatile uint32_t duty_a_q15, duty_b_q15;
static volatile uint16_t operating_i1_delta, operating_i2_delta;
static volatile uint32_t duty_limit_enabled;

#define FAST_DUTY_LIMIT_DEC_Q15 32UL
#define FAST_DUTY_RECOVER_INC_Q15 1UL

static void set_sample_time(ADC_TypeDef *adc, uint32_t channel)
{
    uint32_t shift = channel * 3UL;
    adc->SMPR1 = (adc->SMPR1 & ~(7UL << shift)) |
                 (FAST_ADC_SAMPLE_CODE_47P5 << shift);
}

static void stop_injected_group(ADC_TypeDef *adc)
{
    uint32_t timeout = FAST_ADC_STOP_TIMEOUT_LOOPS;
    if ((adc->CR & ADC_CR_JADSTART) != 0UL) {
        adc->CR |= ADC_CR_JADSTP;
        while ((adc->CR & ADC_CR_JADSTART) != 0UL && timeout-- != 0UL) {
            if ((timeout & 0x3FFUL) == 0UL) power_control_watchdog_kick();
        }
    }
}

static uint32_t abs_delta_u16(uint16_t value, uint16_t zero)
{
    return (value >= zero) ? (uint32_t)(value - zero) :
                             (uint32_t)(zero - value);
}

static void publish_cycle(void)
{
    uint32_t faults = 0UL;
    uint32_t limited = 0UL;
    uint16_t i1 = stage_i1, i2 = stage_i2;
    uint16_t ntc1 = stage_ntc1, ntc2 = stage_ntc2;
    uint16_t vpv = stage_vpv, vbus = stage_vbus;
    if (i1 >= FAST_ADC_RAIL_RAW || i2 >= FAST_ADC_RAIL_RAW ||
        vpv >= FAST_ADC_RAIL_RAW || vbus >= FAST_ADC_RAIL_RAW)
        faults |= BOARD_FAST_ADC_FAULT_RAIL;
    /* The CC6937 conditioner is unipolar and its calibrated zero is near
     * raw 10. Low raw current is therefore normal at zero load; protection is
     * provided by the calibrated delta OCP checks below. */
    if (limit_pv != 0U && vpv >= limit_pv)
        faults |= BOARD_FAST_ADC_FAULT_PV_OVP;
    if (limit_vbus != 0U && vbus >= limit_vbus)
        faults |= BOARD_FAST_ADC_FAULT_VBUS_OVP;
    if (limit_current_valid != 0UL) {
        if (limit_i1_delta != 0U &&
            abs_delta_u16(i1, limit_i1_zero) >= limit_i1_delta)
            faults |= BOARD_FAST_ADC_FAULT_I1_OCP;
        if (limit_i2_delta != 0U &&
            abs_delta_u16(i2, limit_i2_zero) >= limit_i2_delta)
            faults |= BOARD_FAST_ADC_FAULT_I2_OCP;
    }
    if (duty_limit_enabled != 0UL &&
        board_hrtim_power_outputs_enabled() != 0UL) {
        if (duty_a_q15 == 0UL || duty_a_q15 > duty_command_q15)
            duty_a_q15 = duty_command_q15;
        if (duty_b_q15 == 0UL || duty_b_q15 > duty_command_q15)
            duty_b_q15 = duty_command_q15;
        if (operating_i1_delta != 0U &&
            abs_delta_u16(i1, limit_i1_zero) >= operating_i1_delta) {
            duty_a_q15 = (duty_a_q15 > FAST_DUTY_LIMIT_DEC_Q15) ?
                duty_a_q15 - FAST_DUTY_LIMIT_DEC_Q15 : 1UL;
            limited = 1UL;
        } else if (duty_a_q15 < duty_command_q15) {
            duty_a_q15 += FAST_DUTY_RECOVER_INC_Q15;
        }
        if (operating_i2_delta != 0U &&
            abs_delta_u16(i2, limit_i2_zero) >= operating_i2_delta) {
            duty_b_q15 = (duty_b_q15 > FAST_DUTY_LIMIT_DEC_Q15) ?
                duty_b_q15 - FAST_DUTY_LIMIT_DEC_Q15 : 1UL;
            limited = 1UL;
        } else if (duty_b_q15 < duty_command_q15) {
            duty_b_q15 += FAST_DUTY_RECOVER_INC_Q15;
        }
        if (board_hrtim_set_phase_duty_q15(duty_a_q15, duty_b_q15) == 0UL)
            faults |= BOARD_FAST_ADC_FAULT_SEQUENCE;
    } else {
        duty_a_q15 = duty_command_q15;
        duty_b_q15 = duty_command_q15;
    }
    ++snapshot_lock;
    __DMB();
    fast_snapshot.i1_raw = i1;
    fast_snapshot.i2_raw = i2;
    fast_snapshot.ntc1_raw = ntc1;
    fast_snapshot.ntc2_raw = ntc2;
    fast_snapshot.vpv_raw = vpv;
    fast_snapshot.vbus_raw = vbus;
    ++fast_snapshot.sequence;
    fast_snapshot.fault_flags |= faults;
    fast_snapshot.running = (faults == 0UL) ?
                            board_hrtim_sampling_is_running() : 0UL;
    fast_snapshot.current_limit_active = limited;
    fast_snapshot.duty_a_q15 = duty_a_q15;
    fast_snapshot.duty_b_q15 = duty_b_q15;
    __DMB();
    ++snapshot_lock;
    stage_mask = 0UL;
    if (faults != 0UL) board_safety_force_power_off();
}

uint32_t board_fast_adc_init(void)
{
    uint32_t jsqr_common;
    if ((ADC1->CR & ADC_CR_ADEN) == 0UL ||
        (ADC2->CR & ADC_CR_ADEN) == 0UL ||
        board_hrtim_backend_ready() == 0UL) return 0UL;
    board_fast_adc_stop();
    set_sample_time(ADC1, 1UL); set_sample_time(ADC1, 2UL);
    set_sample_time(ADC1, 4UL);
    set_sample_time(ADC2, 6UL); set_sample_time(ADC2, 7UL);
    set_sample_time(ADC2, 3UL);
    /* Three injected ranks per ADC complete in about 4.3 us at the configured
     * 42.5-MHz ADC clock and 47.5-cycle sample time, safely inside the 10-us
     * PWM period. Keeping both NTC channels in this synchronous sequence
     * prevents regular conversions from colliding with the 100-kHz trigger. */
    jsqr_common = ADC_JSQR_JL_1 |
                   FAST_ADC_JEXTSEL_HRTIM_TRG2 |
                   ADC_JSQR_JEXTEN_0;
    ADC1->JSQR = jsqr_common | (1UL << ADC_JSQR_JSQ1_Pos) |
                  (2UL << ADC_JSQR_JSQ2_Pos) |
                  (4UL << ADC_JSQR_JSQ3_Pos);
    ADC2->JSQR = jsqr_common | (6UL << ADC_JSQR_JSQ1_Pos) |
                  (7UL << ADC_JSQR_JSQ2_Pos) |
                  (3UL << ADC_JSQR_JSQ3_Pos);
    ADC1->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS | ADC_ISR_JQOVF | ADC_ISR_OVR;
    ADC2->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS | ADC_ISR_JQOVF | ADC_ISR_OVR;
    stage_mask = 0UL;
    snapshot_lock = 0UL;
    fast_snapshot.sequence = 0UL;
    fast_snapshot.irq_count = 0UL;
    fast_snapshot.incomplete_count = 0UL;
    fast_snapshot.start_count = 0UL;
    fast_snapshot.start_failure_count = 0UL;
    fast_snapshot.armed_mask = 0UL;
    fast_snapshot.last_start_adc1_cr = ADC1->CR;
    fast_snapshot.last_start_adc2_cr = ADC2->CR;
    fast_snapshot.fault_flags = 0UL;
    fast_snapshot.running = 0UL;
    fast_snapshot.current_limit_active = 0UL;
    fast_snapshot.duty_a_q15 = 0UL;
    fast_snapshot.duty_b_q15 = 0UL;
    duty_command_q15 = 0UL;
    duty_a_q15 = 0UL;
    duty_b_q15 = 0UL;
    operating_i1_delta = 0U;
    operating_i2_delta = 0U;
    duty_limit_enabled = 0UL;
    return 1UL;
}

uint32_t board_fast_adc_start(void)
{
    uint32_t start_sequence;
    uint32_t timeout = FAST_ADC_FIRST_CYCLE_TIMEOUT_LOOPS;
    uint32_t armed_mask;
    if ((ADC1->CR & ADC_CR_ADEN) == 0UL ||
        (ADC2->CR & ADC_CR_ADEN) == 0UL ||
        board_hrtim_backend_ready() == 0UL) return 0UL;
    /* Start a fresh sampling epoch so a stopped run cannot carry a stale
     * sequence/rail bit into a new ARM attempt. */
    board_fast_adc_stop();
    board_fast_adc_clear_fault_flags();
    stage_mask = 0UL;
    start_sequence = fast_snapshot.sequence;
    ADC1->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS | ADC_ISR_JQOVF | ADC_ISR_OVR;
    ADC2->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS | ADC_ISR_JQOVF | ADC_ISR_OVR;
    ADC1->IER |= ADC_IER_JEOSIE;
    ADC2->IER |= ADC_IER_JEOSIE;
    NVIC_SetPriority(ADC1_2_IRQn, BOARD_FAST_ADC_IRQ_PRIORITY);
    NVIC_ClearPendingIRQ(ADC1_2_IRQn);
    NVIC_EnableIRQ(ADC1_2_IRQn);
    /* JADSTART is required even with an external trigger.  In this mode it
     * arms the injected group; conversion begins on the next HRTIM_TRG2 edge.
     * This is the same sequence used by ST HAL_ADCEx_InjectedStart_IT(). */
    ADC2->CR |= ADC_CR_JADSTART;
    ADC1->CR |= ADC_CR_JADSTART;
    __DSB();
    armed_mask = (((ADC1->CR & ADC_CR_JADSTART) != 0UL) ? 1UL : 0UL) |
                 (((ADC2->CR & ADC_CR_JADSTART) != 0UL) ? 2UL : 0UL);
    fast_snapshot.armed_mask = armed_mask;
    fast_snapshot.last_start_adc1_cr = ADC1->CR;
    fast_snapshot.last_start_adc2_cr = ADC2->CR;
    if (armed_mask != 3UL) {
        ++fast_snapshot.start_failure_count;
        board_fast_adc_stop();
        return 0UL;
    }
    if (board_hrtim_sampling_start() == 0UL) {
        ++fast_snapshot.start_failure_count;
        board_fast_adc_stop();
        return 0UL;
    }
    fast_snapshot.running = 1UL;
    /* Power outputs remain physically blocked at this point.  Do not report a
     * usable sampling backend until both ADC injected sequences have completed
     * at least one common HRTIM-triggered cycle. */
    while (fast_snapshot.sequence == start_sequence &&
           fast_snapshot.fault_flags == 0UL && timeout-- != 0UL) {
        if ((timeout & 0x3FFUL) == 0UL) power_control_watchdog_kick();
        __NOP();
    }
    if (fast_snapshot.sequence == start_sequence ||
        fast_snapshot.fault_flags != 0UL) {
        ++fast_snapshot.start_failure_count;
        board_fast_adc_trip_sequence_timeout();
        return 0UL;
    }
    ++fast_snapshot.start_count;
    return 1UL;
}

void board_fast_adc_stop(void)
{
    board_hrtim_sampling_stop();
    stop_injected_group(ADC1);
    stop_injected_group(ADC2);
    NVIC_DisableIRQ(ADC1_2_IRQn);
    ADC1->IER &= ~ADC_IER_JEOSIE;
    ADC2->IER &= ~ADC_IER_JEOSIE;
    ADC1->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS | ADC_ISR_JQOVF | ADC_ISR_OVR;
    ADC2->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS | ADC_ISR_JQOVF | ADC_ISR_OVR;
    NVIC_ClearPendingIRQ(ADC1_2_IRQn);
    fast_snapshot.armed_mask = 0UL;
    fast_snapshot.running = 0UL;
}

uint32_t board_fast_adc_get_snapshot(board_fast_adc_snapshot_t *out)
{
    uint32_t begin, end, attempts = 4UL;
    if (out == (board_fast_adc_snapshot_t *)0) return 0UL;
    do {
        begin = snapshot_lock;
        __DMB();
        *out = fast_snapshot;
        __DMB();
        end = snapshot_lock;
        if (begin == end && (begin & 1UL) == 0UL)
            return (out->sequence != 0UL) ? 1UL : 0UL;
    } while (attempts-- != 0UL);
    return 0UL;
}

void board_fast_adc_set_limits(uint16_t i1_zero_raw, uint16_t i2_zero_raw,
                               uint16_t i1_delta_raw, uint16_t i2_delta_raw,
                               uint16_t pv_ovp_raw, uint16_t vbus_ovp_raw,
                               uint32_t current_limit_valid)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    limit_i1_zero = i1_zero_raw;
    limit_i2_zero = i2_zero_raw;
    limit_i1_delta = i1_delta_raw;
    limit_i2_delta = i2_delta_raw;
    limit_pv = pv_ovp_raw;
    limit_vbus = vbus_ovp_raw;
    limit_current_valid = current_limit_valid;
    __DMB();
    if (state == 0UL) __enable_irq();
}

void board_fast_adc_set_duty_limit_command(uint32_t duty_q15,
                                           uint16_t i1_operating_delta_raw,
                                           uint16_t i2_operating_delta_raw,
                                           uint32_t enabled)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    duty_command_q15 = duty_q15;
    operating_i1_delta = i1_operating_delta_raw;
    operating_i2_delta = i2_operating_delta_raw;
    duty_limit_enabled = enabled;
    if (enabled == 0UL) {
        duty_a_q15 = duty_q15;
        duty_b_q15 = duty_q15;
    }
    __DMB();
    if (state == 0UL) __enable_irq();
}

uint32_t board_fast_adc_fault_flags(void) { return fast_snapshot.fault_flags; }

void board_fast_adc_clear_fault_flags(void)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    fast_snapshot.fault_flags = 0UL;
    if (state == 0UL) __enable_irq();
}

void board_fast_adc_trip_sequence_timeout(void)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    ++snapshot_lock;
    __DMB();
    fast_snapshot.fault_flags |= BOARD_FAST_ADC_FAULT_SEQUENCE;
    ++fast_snapshot.incomplete_count;
    fast_snapshot.running = 0UL;
    __DMB();
    ++snapshot_lock;
    stage_mask = 0UL;
    board_safety_force_power_off();
    if (state == 0UL) __enable_irq();
}

void ADC1_2_IRQHandler(void)
{
    uint32_t adc1_status = ADC1->ISR;
    uint32_t adc2_status = ADC2->ISR;
    uint32_t sequence_error =
        ((adc1_status & (ADC_ISR_JQOVF | ADC_ISR_OVR)) != 0UL ||
         (adc2_status & (ADC_ISR_JQOVF | ADC_ISR_OVR)) != 0UL) ? 1UL : 0UL;
    ++fast_snapshot.irq_count;
    if ((adc1_status & ADC_ISR_JEOS) != 0UL) {
        if ((stage_mask & 1UL) != 0UL) sequence_error = 1UL;
        stage_i1 = (uint16_t)(ADC1->JDR1 & 0x0FFFUL);
        stage_i2 = (uint16_t)(ADC1->JDR2 & 0x0FFFUL);
        stage_ntc1 = (uint16_t)(ADC1->JDR3 & 0x0FFFUL);
        ADC1->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS;
        stage_mask |= 1UL;
    }
    if ((adc2_status & ADC_ISR_JEOS) != 0UL) {
        if ((stage_mask & 2UL) != 0UL) sequence_error = 1UL;
        stage_vpv = (uint16_t)(ADC2->JDR1 & 0x0FFFUL);
        stage_vbus = (uint16_t)(ADC2->JDR2 & 0x0FFFUL);
        stage_ntc2 = (uint16_t)(ADC2->JDR3 & 0x0FFFUL);
        ADC2->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS;
        stage_mask |= 2UL;
    }
    if (sequence_error != 0UL) {
        ADC1->ISR = ADC_ISR_JQOVF | ADC_ISR_OVR;
        ADC2->ISR = ADC_ISR_JQOVF | ADC_ISR_OVR;
        ++snapshot_lock;
        __DMB();
        fast_snapshot.fault_flags |= BOARD_FAST_ADC_FAULT_SEQUENCE;
        ++fast_snapshot.incomplete_count;
        fast_snapshot.running = 0UL;
        __DMB();
        ++snapshot_lock;
        stage_mask = 0UL;
        board_safety_force_power_off();
        return;
    }
    if (stage_mask == 3UL) publish_cycle();
}
