#include "board_hrtim.h"
#include "board_config.h"
#include "board_clock.h"
#include "stm32g474_bare.h"

extern void power_control_watchdog_kick(void);

#define HRTIM_COUNTER_ENABLE_MASK \
    (HRTIM_MCR_MCEN | HRTIM_MCR_TACEN | HRTIM_MCR_TBCEN | \
     HRTIM_MCR_TCCEN | HRTIM_MCR_TDCEN | HRTIM_MCR_TECEN | \
     HRTIM_MCR_TFCEN)

static uint32_t timing_configured;
static uint32_t configured_compare_ticks;
static uint32_t dll_ready;
static uint32_t sampling_running;
static uint32_t power_outputs_enabled;
static uint32_t arm_attempts;
static uint32_t arm_fail_stage;
static uint32_t output_enable_readback;
static uint32_t internal_seen_high;
static uint32_t internal_seen_low;
static uint32_t pad_seen_high;
static uint32_t pad_seen_low;
static uint32_t arm_gate_fail_mask;

/* board_hrtim_force_off() is deliberately destructive for the active HRTIM
 * state, but clearing the commissioning evidence made every arming failure
 * look like stage=0 in HRTIMDIAG.  Keep the last attempt in a separate,
 * read-only snapshot so a failed START remains diagnosable after containment.
 */
static uint32_t last_arm_attempts;
static uint32_t last_arm_fail_stage;
static uint32_t last_output_enable_readback;
static uint32_t last_internal_seen_high;
static uint32_t last_internal_seen_low;
static uint32_t last_pad_seen_high;
static uint32_t last_pad_seen_low;
static uint32_t last_arm_gate_fail_mask;

static void remember_arm_diagnostics(void)
{
    last_arm_attempts = arm_attempts;
    last_arm_fail_stage = arm_fail_stage;
    last_output_enable_readback = output_enable_readback;
    last_internal_seen_high = internal_seen_high;
    last_internal_seen_low = internal_seen_low;
    last_pad_seen_high = pad_seen_high;
    last_pad_seen_low = pad_seen_low;
    last_arm_gate_fail_mask = arm_gate_fail_mask;
}

#define NONSYNC_OUTPUT_ENABLE_MASK (HRTIM_OENR_TA2OEN | HRTIM_OENR_TB1OEN)
#define NONSYNC_OUTPUT_DISABLE_MASK (HRTIM_ODISR_TA2ODIS | HRTIM_ODISR_TB1ODIS)
#define NONSYNC_EDGE_CHE2 (1UL << 0)
#define NONSYNC_EDGE_CHF1 (1UL << 1)
#define NONSYNC_EDGE_BOTH (NONSYNC_EDGE_CHE2 | NONSYNC_EDGE_CHF1)
#define HRTIM_EDGE_SELFTEST_LOOPS 200000UL

/* HRTIM ADC trigger registers are shadowed.  ADC2R is transferred to the
 * active trigger register by the selected update source in CR1.  The fast
 * sampler starts Timer A (not the master timer), therefore ADC2 must use the
 * Timer-A update source.  The HAL expresses this as
 * (HRTIM_ADCTRIGGERUPDATE_TIMER_A << 3), which is exactly
 * HRTIM_CR1_ADC2USRC_0 for ADC trigger 2. */
#define HRTIM_ADC2_UPDATE_TIMER_A HRTIM_CR1_ADC2USRC_0

static void gpio_mask_to_analog(GPIO_TypeDef *port, uint32_t mask)
{
    uint32_t pin;
    for (pin = 0U; pin < 16U; ++pin) {
        if ((mask & (1UL << pin)) != 0UL) {
            uint32_t shift = pin * 2U;
            port->MODER |= 3UL << shift;
            port->PUPDR &= ~(3UL << shift);
        }
    }
}

static uint32_t gpio_mask_is_analog(GPIO_TypeDef *port, uint32_t mask)
{
    uint32_t pin;
    for (pin = 0U; pin < 16U; ++pin) {
        if ((mask & (1UL << pin)) != 0UL &&
            ((port->MODER >> (pin * 2U)) & 3UL) != 3UL) return 0UL;
    }
    return 1UL;
}

static void gpio_pin_to_af13(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t shift = pin * 2U;
    uint32_t afr_index = pin >> 3;
    uint32_t afr_shift = (pin & 7U) * 4U;
    port->AFR[afr_index] = (port->AFR[afr_index] & ~(0xFUL << afr_shift)) |
                           (13UL << afr_shift);
    port->MODER = (port->MODER & ~(3UL << shift)) | (2UL << shift);
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << shift)) | (3UL << shift);
    port->PUPDR &= ~(3UL << shift);
}

static uint32_t gpio_pin_is_af13(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t afr_index = pin >> 3;
    uint32_t afr_shift = (pin & 7U) * 4U;
    return ((((port->MODER >> (pin * 2U)) & 3UL) == 2UL) &&
            (((port->AFR[afr_index] >> afr_shift) & 0xFUL) == 13UL)) ? 1UL : 0UL;
}

static uint32_t timing_contract_readback(void)
{
    const HRTIM_Timerx_TypeDef *timer_a = &HRTIM1->sTimerxRegs[0];
    const HRTIM_Timerx_TypeDef *timer_b = &HRTIM1->sTimerxRegs[1];
    if (timer_a->PERxR != BOARD_HRTIM_PERIOD_TICKS ||
        timer_b->PERxR != BOARD_HRTIM_PERIOD_TICKS) return 0UL;
    if ((timer_a->TIMxCR & HRTIM_TIMCR_CK_PSC) !=
            (BOARD_HRTIM_PRESCALER_CODE & HRTIM_TIMCR_CK_PSC) ||
        (timer_b->TIMxCR & HRTIM_TIMCR_CK_PSC) !=
            (BOARD_HRTIM_PRESCALER_CODE & HRTIM_TIMCR_CK_PSC)) return 0UL;
    if (timer_a->CMP1xR != configured_compare_ticks ||
        timer_b->CMP1xR != configured_compare_ticks) return 0UL;
    if (timer_a->SETx2R != HRTIM_SET2R_PER ||
        timer_a->RSTx2R != HRTIM_RST2R_CMP1) return 0UL;
    if (timer_b->SETx1R != HRTIM_SET1R_PER ||
        timer_b->RSTx1R != HRTIM_RST1R_CMP1) return 0UL;
    if (HRTIM1->sCommonRegs.ADC2R != HRTIM_ADC2R_AD2TAC2) return 0UL;
    if ((HRTIM1->sCommonRegs.CR1 & HRTIM_CR1_ADC2USRC) !=
        HRTIM_ADC2_UPDATE_TIMER_A) return 0UL;
    return 1UL;
}

static void configure_stopped_timing_contract(void)
{
    HRTIM_Timerx_TypeDef *timer_a = &HRTIM1->sTimerxRegs[0];
    HRTIM_Timerx_TypeDef *timer_b = &HRTIM1->sTimerxRegs[1];

    /* 54,400 * 32,767 fits in uint32_t.  Keep the 100-kHz duty path free of
     * run-time 64-bit division helpers on Cortex-M4. */
    configured_compare_ticks =
        (((BOARD_HRTIM_PERIOD_TICKS + 1UL) *
          BOARD_HRTIM_INITIAL_DUTY_Q15) + 16384UL) >> 15;

    /* CHE2 is PA9/HRTIM1_CHA2: Timer A output 2. */
    timer_a->TIMxCR =
        (BOARD_HRTIM_PRESCALER_CODE & HRTIM_TIMCR_CK_PSC) |
        HRTIM_TIMCR_CONT | HRTIM_TIMCR_PREEN;
    timer_a->PERxR = BOARD_HRTIM_PERIOD_TICKS;
    timer_a->CMP1xR = configured_compare_ticks;
    timer_a->CMP2xR = (BOARD_HRTIM_PERIOD_TICKS + 1UL) / 2UL;
    timer_a->SETx2R = HRTIM_SET2R_PER;
    timer_a->RSTx2R = HRTIM_RST2R_CMP1;
    /* Explicit output contract: active-high, inactive idle/fault level,
     * chopper/dead-time/delayed-protection disabled.  Do not rely on reset
     * defaults when this module may be re-entered after a debugger reset. */
    timer_a->OUTxR = 0UL;
    timer_a->DTxR = 0UL;
    timer_a->CHPxR = 0UL;
    timer_a->RSTxR = 0UL;
    timer_a->FLTxR = 0UL;

    /* CHF1 is PA10/HRTIM1_CHB1: Timer B output 1. */
    timer_b->TIMxCR =
        (BOARD_HRTIM_PRESCALER_CODE & HRTIM_TIMCR_CK_PSC) |
        HRTIM_TIMCR_CONT | HRTIM_TIMCR_PREEN;
    timer_b->PERxR = BOARD_HRTIM_PERIOD_TICKS;
    timer_b->CMP1xR = configured_compare_ticks;
    timer_b->CMP2xR = (BOARD_HRTIM_PERIOD_TICKS + 1UL) / 2UL;
    timer_b->SETx1R = HRTIM_SET1R_PER;
    timer_b->RSTx1R = HRTIM_RST1R_CMP1;
    timer_b->OUTxR = 0UL;
    timer_b->DTxR = 0UL;
    timer_b->CHPxR = 0UL;
    timer_b->RSTxR = 0UL;
    timer_b->FLTxR = 0UL;

    /* One trigger per PWM period.  ADC2R is a preload/shadow register, so
     * explicitly select Timer A as its update source before issuing the
     * software update below.  Without this field the active ADC trigger stays
     * on the reset-default master-timer source; Timer-A-only sampling then
     * produces no JEOS interrupt and the control state machine must fail safe. */
    HRTIM1->sCommonRegs.ADC1R = 0UL;
    HRTIM1->sCommonRegs.ADC2R = HRTIM_ADC2R_AD2TAC2;
    HRTIM1->sCommonRegs.ADC3R = 0UL;
    HRTIM1->sCommonRegs.ADC4R = 0UL;
    HRTIM1->sCommonRegs.CR1 =
        (HRTIM1->sCommonRegs.CR1 & ~HRTIM_CR1_ADC2USRC) |
        HRTIM_ADC2_UPDATE_TIMER_A;
    HRTIM1->sCommonRegs.FLTINR1 = 0UL;
    HRTIM1->sCommonRegs.FLTINR2 = 0UL;
    HRTIM1->sCommonRegs.FLTINR3 = 0UL;
    HRTIM1->sCommonRegs.FLTINR4 = 0UL;
    HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_TASWU | HRTIM_CR2_TBSWU;
    timing_configured = timing_contract_readback();
}

void board_hrtim_force_off(void)
{
    HRTIM1->sCommonRegs.ODISR = HRTIM_OUTPUT_ALL;
    HRTIM1->sMasterRegs.MCR &= ~HRTIM_COUNTER_ENABLE_MASK;
    sampling_running = 0UL;
    power_outputs_enabled = 0UL;
    arm_attempts = 0UL;
    arm_fail_stage = 0UL;
    arm_gate_fail_mask = 0UL;
    output_enable_readback = 0UL;
    internal_seen_high = 0UL;
    internal_seen_low = 0UL;
    pad_seen_high = 0UL;
    pad_seen_low = 0UL;

    /* Remove every HRTIM alternate function as an independent containment
     * layer. This also makes debugger-only resets deterministic. */
    gpio_mask_to_analog(GPIOA, HRTIM_PA_OUTPUT_MASK);
    gpio_mask_to_analog(GPIOB, HRTIM_PB_OUTPUT_MASK);
    gpio_mask_to_analog(GPIOC, HRTIM_PC_OUTPUT_MASK);
    __DSB();
}

static uint32_t calibrate_dll(void)
{
    uint32_t timeout = BOARD_HRTIM_DLL_TIMEOUT_LOOPS;
    if (board_clock_is_170mhz() == 0UL) return 0UL;
    HRTIM1->sCommonRegs.ICR = HRTIM_ICR_DLLRDYC;
    HRTIM1->sCommonRegs.DLLCR = HRTIM_DLLCR_CALEN |
        HRTIM_DLLCR_CALRTE_1 | HRTIM_DLLCR_CALRTE_0 | HRTIM_DLLCR_CAL;
    while ((HRTIM1->sCommonRegs.ISR & HRTIM_ISR_DLLRDY) == 0UL &&
           timeout-- != 0UL) {
        if ((timeout & 0x3FFUL) == 0UL) power_control_watchdog_kick();
    }
    return ((HRTIM1->sCommonRegs.ISR & HRTIM_ISR_DLLRDY) != 0UL) ? 1UL : 0UL;
}

void board_hrtim_safe_init(void)
{
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN |
                   RCC_AHB2ENR_GPIOCEN;
    RCC_APB2ENR |= RCC_APB2ENR_HRTIM1EN;
    (void)RCC_APB2ENR;

    RCC->APB2RSTR |= RCC_APB2RSTR_HRTIM1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_HRTIM1RST;
    timing_configured = 0UL;
    configured_compare_ticks = 0UL;
    dll_ready = 0UL;
    sampling_running = 0UL;
    power_outputs_enabled = 0UL;
    last_arm_attempts = 0UL;
    last_arm_fail_stage = 0UL;
    last_output_enable_readback = 0UL;
    last_internal_seen_high = 0UL;
    last_internal_seen_low = 0UL;
    last_pad_seen_high = 0UL;
    last_pad_seen_low = 0UL;
    last_arm_gate_fail_mask = 0UL;
    board_hrtim_force_off();
#if BOARD_HRTIM_TIMING_CONFIG_ENABLE
    configure_stopped_timing_contract();
    dll_ready = calibrate_dll();
#endif
    board_hrtim_force_off();
}

uint32_t board_hrtim_backend_ready(void)
{
    return (timing_configured != 0UL && dll_ready != 0UL &&
            board_clock_is_170mhz() != 0UL) ? 1UL : 0UL;
}

uint32_t board_hrtim_sampling_start(void)
{
    if (board_hrtim_backend_ready() == 0UL) return 0UL;
    board_hrtim_force_off();
    HRTIM1->sCommonRegs.ODISR = HRTIM_OUTPUT_ALL;
    HRTIM1->sMasterRegs.MCR |= HRTIM_MCR_TACEN;
    __DSB();
    sampling_running =
        ((HRTIM1->sMasterRegs.MCR & HRTIM_MCR_TACEN) != 0UL &&
         gpio_mask_is_analog(GPIOA, HRTIM_PA_OUTPUT_MASK) != 0UL &&
         gpio_mask_is_analog(GPIOB, HRTIM_PB_OUTPUT_MASK) != 0UL &&
         gpio_mask_is_analog(GPIOC, HRTIM_PC_OUTPUT_MASK) != 0UL) ? 1UL : 0UL;
    return sampling_running;
}

void board_hrtim_sampling_stop(void)
{
    HRTIM1->sMasterRegs.MCR &= ~HRTIM_MCR_TACEN;
    sampling_running = 0UL;
}

uint32_t board_hrtim_sampling_is_running(void)
{
    return sampling_running;
}

uint32_t board_hrtim_set_duty_q15(uint32_t duty_q15)
{
    return board_hrtim_set_phase_duty_q15(duty_q15, duty_q15);
}

static uint32_t duty_to_ticks(uint32_t duty_q15)
{
    uint32_t ticks = (((BOARD_HRTIM_PERIOD_TICKS + 1UL) * duty_q15) +
                       16384UL) >> 15;
    if (ticks < 3UL) ticks = 3UL;
    if (ticks > BOARD_HRTIM_PERIOD_TICKS - 3UL)
        ticks = BOARD_HRTIM_PERIOD_TICKS - 3UL;
    return ticks;
}

uint32_t board_hrtim_set_phase_duty_q15(uint32_t duty_a_q15,
                                        uint32_t duty_b_q15)
{
    uint32_t ticks_a, ticks_b;
    if (board_hrtim_backend_ready() == 0UL || duty_a_q15 == 0UL ||
        duty_b_q15 == 0UL || duty_a_q15 >= 32768UL ||
        duty_b_q15 >= 32768UL) return 0UL;
    ticks_a = duty_to_ticks(duty_a_q15);
    ticks_b = duty_to_ticks(duty_b_q15);
    HRTIM1->sTimerxRegs[0].CMP1xR = ticks_a;
    HRTIM1->sTimerxRegs[1].CMP1xR = ticks_b;
    HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_TASWU | HRTIM_CR2_TBSWU;
    configured_compare_ticks = ticks_a;
    __DMB();
    return (HRTIM1->sTimerxRegs[0].CMP1xR == ticks_a &&
            HRTIM1->sTimerxRegs[1].CMP1xR == ticks_b) ? 1UL : 0UL;
}

/* A board-specific implementation may override this only after it has
 * configured and read back FLTxR/FLTINRx using measured source polarity,
 * filter and latch behavior. Connector names or macros are not evidence. */
__attribute__((weak)) uint32_t board_hrtim_fault_backend_ready(void)
{
    return 0UL;
}

uint32_t board_hrtim_power_arm(uint32_t runtime_interlocks_ok)
{
    HRTIM_Timerx_TypeDef *timer_a = &HRTIM1->sTimerxRegs[0];
    HRTIM_Timerx_TypeDef *timer_b = &HRTIM1->sTimerxRegs[1];
    uint32_t timeout;
    uint32_t levels;
    ++arm_attempts;
    arm_fail_stage = 0UL;
    output_enable_readback = 0UL;
    internal_seen_high = 0UL;
    internal_seen_low = 0UL;
    pad_seen_high = 0UL;
    pad_seen_low = 0UL;
    if (BOARD_POWER_OUTPUT_ARMING_ENABLE == 0U ||
#if !BOARD_EXTERNAL_PROTECTION_CONFIRMED
        BOARD_HARDWARE_FLT_CONFIRMED == 0U ||
        BOARD_V15_FEEDBACK_CONFIRMED == 0U ||
        BOARD_V15_PGOOD_RUNTIME_IMPLEMENTED == 0U ||
        BOARD_FLT_RUNTIME_POLARITY_VALIDATED == 0U ||
        board_hrtim_fault_backend_ready() == 0UL ||
#endif
        runtime_interlocks_ok == 0UL || board_hrtim_backend_ready() == 0UL) {
        arm_fail_stage = 1UL;
        /* Bit 0=arming compiled, bit 1=runtime interlocks, bit 2=HRTIM
         * backend.  Keep this evidence across the destructive off path. */
        arm_gate_fail_mask = 0UL;
        if (BOARD_POWER_OUTPUT_ARMING_ENABLE == 0U) arm_gate_fail_mask |= 1UL << 0;
        if (runtime_interlocks_ok == 0UL) arm_gate_fail_mask |= 1UL << 1;
        if (board_hrtim_backend_ready() == 0UL) arm_gate_fail_mask |= 1UL << 2;
#if !BOARD_EXTERNAL_PROTECTION_CONFIRMED
        if (BOARD_HARDWARE_FLT_CONFIRMED == 0U) arm_gate_fail_mask |= 1UL << 3;
        if (BOARD_V15_FEEDBACK_CONFIRMED == 0U) arm_gate_fail_mask |= 1UL << 4;
        if (BOARD_V15_PGOOD_RUNTIME_IMPLEMENTED == 0U) arm_gate_fail_mask |= 1UL << 5;
        if (BOARD_FLT_RUNTIME_POLARITY_VALIDATED == 0U) arm_gate_fail_mask |= 1UL << 6;
        if (board_hrtim_fault_backend_ready() == 0UL) arm_gate_fail_mask |= 1UL << 7;
#endif
        remember_arm_diagnostics();
        return 0UL;
    }

    HRTIM1->sCommonRegs.ODISR = HRTIM_OUTPUT_ALL;
    HRTIM1->sMasterRegs.MCR &= ~HRTIM_COUNTER_ENABLE_MASK;
    timer_a->CNTxR = 0UL;
    timer_b->CNTxR = (BOARD_HRTIM_PERIOD_TICKS + 1UL) / 2UL;
    /* Keep OE3 high and PA2 asserted in board_safety while the raw MCU pads
     * are tested.  Configure AF and HRTIM output gates before starting the
     * counters, matching ST's waveform output/counter start sequence. */
    gpio_pin_to_af13(GPIOA, BOARD_NONSYNC_CHE2_PIN);
    gpio_pin_to_af13(GPIOA, BOARD_NONSYNC_CHF1_PIN);
    HRTIM1->sCommonRegs.OENR = NONSYNC_OUTPUT_ENABLE_MASK;
    __DSB();
    output_enable_readback = HRTIM1->sCommonRegs.OENR &
                             NONSYNC_OUTPUT_ENABLE_MASK;
    if (output_enable_readback != NONSYNC_OUTPUT_ENABLE_MASK ||
        gpio_pin_is_af13(GPIOA, BOARD_NONSYNC_CHE2_PIN) == 0UL ||
        gpio_pin_is_af13(GPIOA, BOARD_NONSYNC_CHF1_PIN) == 0UL) {
        arm_fail_stage = 2UL;
        remember_arm_diagnostics();
        board_hrtim_force_off();
        return 0UL;
    }
    HRTIM1->sMasterRegs.MCR |= HRTIM_MCR_TACEN | HRTIM_MCR_TBCEN;
    __DSB();
    if ((HRTIM1->sMasterRegs.MCR &
         (HRTIM_MCR_TACEN | HRTIM_MCR_TBCEN)) !=
        (HRTIM_MCR_TACEN | HRTIM_MCR_TBCEN)) {
        arm_fail_stage = 3UL;
        remember_arm_diagnostics();
        board_hrtim_force_off();
        return 0UL;
    }

    /* Observe both the HRTIM internal output state and the actual GPIO input
     * buffers.  AF-mode GPIO input buffers remain readable on STM32G4.  This
     * test runs while the external tri-state buffer is disabled, so it proves
     * PA9/PA10 PWM edges without driving the power stage. */
    timeout = HRTIM_EDGE_SELFTEST_LOOPS;
    while (timeout-- != 0UL) {
        if ((timeout & 0x3FFUL) == 0UL) power_control_watchdog_kick();
        levels = 0UL;
        if ((timer_a->TIMxISR & HRTIM_TIMISR_O2STAT) != 0UL)
            levels |= NONSYNC_EDGE_CHE2;
        if ((timer_b->TIMxISR & HRTIM_TIMISR_O1STAT) != 0UL)
            levels |= NONSYNC_EDGE_CHF1;
        internal_seen_high |= levels;
        internal_seen_low |= (~levels) & NONSYNC_EDGE_BOTH;

        levels = 0UL;
        if ((GPIOA->IDR & (1UL << BOARD_NONSYNC_CHE2_PIN)) != 0UL)
            levels |= NONSYNC_EDGE_CHE2;
        if ((GPIOA->IDR & (1UL << BOARD_NONSYNC_CHF1_PIN)) != 0UL)
            levels |= NONSYNC_EDGE_CHF1;
        pad_seen_high |= levels;
        pad_seen_low |= (~levels) & NONSYNC_EDGE_BOTH;
        if (internal_seen_high == NONSYNC_EDGE_BOTH &&
            internal_seen_low == NONSYNC_EDGE_BOTH &&
            pad_seen_high == NONSYNC_EDGE_BOTH &&
            pad_seen_low == NONSYNC_EDGE_BOTH) break;
    }
    if (internal_seen_high != NONSYNC_EDGE_BOTH ||
        internal_seen_low != NONSYNC_EDGE_BOTH) {
        arm_fail_stage = 4UL;
        remember_arm_diagnostics();
        board_hrtim_force_off();
        return 0UL;
    }
#if !BOARD_EXTERNAL_PROTECTION_CONFIRMED
    /* With a commissioned external protection/tri-state path the driver
     * deliberately keeps OE3 asserted during this self-test.  PA9/PA10 are
     * then isolated from the power board and their IDR value is not evidence
     * of an HRTIM failure.  Require pad edges only on the MCU-readable
     * protection profile; the external profile is checked after OE3/PA2 are
     * released by board_safety_request_power_on(). */
    if (pad_seen_high != NONSYNC_EDGE_BOTH ||
        pad_seen_low != NONSYNC_EDGE_BOTH) {
        arm_fail_stage = 5UL;
        remember_arm_diagnostics();
        board_hrtim_force_off();
        return 0UL;
    }
#endif
    power_outputs_enabled = 1UL;
    sampling_running = 1UL;
    remember_arm_diagnostics();
    return 1UL;
}

void board_hrtim_mark_arm_failure(uint32_t stage, uint32_t gate_fail_mask)
{
    arm_fail_stage = stage;
    arm_gate_fail_mask = gate_fail_mask;
    remember_arm_diagnostics();
}

uint32_t board_hrtim_power_outputs_enabled(void)
{
    uint32_t required_counters = HRTIM_MCR_TACEN | HRTIM_MCR_TBCEN;
    if (power_outputs_enabled == 0UL ||
        (HRTIM1->sMasterRegs.MCR & required_counters) != required_counters ||
        (HRTIM1->sCommonRegs.OENR & NONSYNC_OUTPUT_ENABLE_MASK) !=
            NONSYNC_OUTPUT_ENABLE_MASK ||
        gpio_pin_is_af13(GPIOA, BOARD_NONSYNC_CHE2_PIN) == 0UL ||
        gpio_pin_is_af13(GPIOA, BOARD_NONSYNC_CHF1_PIN) == 0UL ||
        internal_seen_high != NONSYNC_EDGE_BOTH ||
        internal_seen_low != NONSYNC_EDGE_BOTH
#if !BOARD_EXTERNAL_PROTECTION_CONFIRMED
        || pad_seen_high != NONSYNC_EDGE_BOTH ||
           pad_seen_low != NONSYNC_EDGE_BOTH
#endif
        )
        return 0UL;
    return 1UL;
}

void board_hrtim_get_diag(board_hrtim_diag_t *diag)
{
    if (diag == (board_hrtim_diag_t *)0) return;
    diag->backend_ready = board_hrtim_backend_ready();
    diag->timing_configured = timing_configured;
    diag->clock_validated = board_clock_is_170mhz();
    diag->requested_pwm_hz = BOARD_HRTIM_REQUESTED_PWM_HZ;
    diag->prescaler_code = BOARD_HRTIM_PRESCALER_CODE;
    diag->protection_ready = board_hrtim_fault_backend_ready();
    diag->arming_compiled = BOARD_POWER_OUTPUT_ARMING_ENABLE;
    diag->profile = BOARD_POWER_STAGE_PROFILE;
    diag->counters_enabled = HRTIM1->sMasterRegs.MCR &
                             HRTIM_COUNTER_ENABLE_MASK;
    diag->output_disable_status = HRTIM1->sCommonRegs.ODSR &
                                  HRTIM_OUTPUT_ALL;
    diag->pwm_pins_high_z =
        gpio_mask_is_analog(GPIOA, HRTIM_PA_OUTPUT_MASK) &&
        gpio_mask_is_analog(GPIOB, HRTIM_PB_OUTPUT_MASK) &&
        gpio_mask_is_analog(GPIOC, HRTIM_PC_OUTPUT_MASK);
    diag->period_ticks = BOARD_HRTIM_PERIOD_TICKS;
    diag->compare_ticks = configured_compare_ticks;
    diag->adc_trigger_configured =
        (HRTIM1->sCommonRegs.ADC2R == HRTIM_ADC2R_AD2TAC2 &&
         (HRTIM1->sCommonRegs.CR1 & HRTIM_CR1_ADC2USRC) ==
         HRTIM_ADC2_UPDATE_TIMER_A) ? 1UL : 0UL;
    diag->dll_ready = dll_ready;
    diag->sampling_running = sampling_running;
    if (diag->clock_validated != 0UL) {
        diag->effective_hr_clock_mhz =
            (BOARD_TARGET_SYSCLK_HZ / 1000000UL) * 32UL;
        diag->calculated_pwm_hz = (BOARD_HRTIM_PERIOD_TICKS != 0UL) ?
            (uint32_t)(((uint64_t)BOARD_TARGET_SYSCLK_HZ * 32ULL) /
                       ((uint64_t)BOARD_HRTIM_PERIOD_TICKS + 1ULL)) : 0UL;
    } else {
        diag->effective_hr_clock_mhz = 0UL;
        diag->calculated_pwm_hz = 0UL;
    }
    diag->dma_configured = 0UL;
    diag->deadtime_applicable = 0UL;
    diag->nonsync_che2_timer_output =
        (BOARD_NONSYNC_CHE2_TIMER_ID << 8) | BOARD_NONSYNC_CHE2_OUTPUT;
    diag->nonsync_chf1_timer_output =
        (BOARD_NONSYNC_CHF1_TIMER_ID << 8) | BOARD_NONSYNC_CHF1_OUTPUT;
    diag->phase_shift_ticks = (BOARD_HRTIM_PERIOD_TICKS + 1UL) / 2UL;
    diag->power_outputs_enabled = power_outputs_enabled;
    diag->arm_attempts = (arm_attempts != 0UL) ?
                         arm_attempts : last_arm_attempts;
    diag->arm_fail_stage = (arm_attempts != 0UL) ?
                           arm_fail_stage : last_arm_fail_stage;
    diag->output_enable_readback = (arm_attempts != 0UL) ?
                                   output_enable_readback :
                                   last_output_enable_readback;
    diag->internal_seen_high = (arm_attempts != 0UL) ?
                               internal_seen_high : last_internal_seen_high;
    diag->internal_seen_low = (arm_attempts != 0UL) ?
                              internal_seen_low : last_internal_seen_low;
    diag->pad_seen_high = (arm_attempts != 0UL) ?
                          pad_seen_high : last_pad_seen_high;
    diag->pad_seen_low = (arm_attempts != 0UL) ?
                          pad_seen_low : last_pad_seen_low;
    diag->timer_a_output_reg = HRTIM1->sTimerxRegs[0].OUTxR;
    diag->timer_b_output_reg = HRTIM1->sTimerxRegs[1].OUTxR;
    diag->arm_gate_fail_mask = (arm_attempts != 0UL) ?
                               arm_gate_fail_mask : last_arm_gate_fail_mask;
}
