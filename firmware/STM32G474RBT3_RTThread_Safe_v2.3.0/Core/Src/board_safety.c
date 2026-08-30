#include "board_safety.h"
#include "board_config.h"
#include "board_hrtim.h"
#include "stm32g474_bare.h"

static void gpio_output(GPIO_TypeDef *port, uint32_t pin, uint32_t high)
{
    uint32_t shift = pin * 2U;
    port->BSRR = high ? (1UL << pin) : (1UL << (pin + 16U));
    port->MODER = (port->MODER & ~(3UL << shift)) | (1UL << shift);
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << shift)) | (2UL << shift);
    port->PUPDR &= ~(3UL << shift);
}

static uint32_t gpio_read(GPIO_TypeDef *port, uint32_t pin)
{
    return ((port->IDR & (1UL << pin)) != 0UL) ? 1UL : 0UL;
}

#if BOARD_FLT_INPUT_CONNECTED
static void gpio_input_pulldown(GPIO_TypeDef *port, uint32_t pin)
{
    uint32_t shift = pin * 2U;
    port->MODER &= ~(3UL << shift);
    port->PUPDR = (port->PUPDR & ~(3UL << shift)) | (2UL << shift);
}

#endif

__attribute__((weak)) uint32_t board_v15_pgood_read(void)
{
    return 0UL;
}

uint32_t board_aux_set_request(uint32_t enabled)
{
    gpio_output(GPIOA, AUX_ENABLE_PIN, enabled != 0UL ? 1UL : 0UL);
    __DSB();
    if (enabled != 0UL)
        return (((GPIOA->ODR & (1UL << AUX_ENABLE_PIN)) != 0UL) &&
                gpio_read(GPIOA, AUX_ENABLE_PIN) != 0UL) ? 1UL : 0UL;
    return (((GPIOA->ODR & (1UL << AUX_ENABLE_PIN)) == 0UL) &&
            gpio_read(GPIOA, AUX_ENABLE_PIN) == 0UL) ? 0UL : 1UL;
}

uint32_t board_aux_get_request(void)
{
    return ((GPIOA->ODR & (1UL << AUX_ENABLE_PIN)) != 0UL) ? 1UL : 0UL;
}

void board_safety_force_power_off(void)
{
    board_hrtim_force_off();
    gpio_output(GPIOB, OE1_PIN, 1UL);
    gpio_output(GPIOC, OE2_PIN, 1UL);
    gpio_output(GPIOC, OE3_PIN, 1UL);
    gpio_output(GPIOA, GATE_INHIBIT_PIN, 1UL);
    __DSB();
}

void board_safety_force_off(void)
{
    board_safety_force_power_off();
    gpio_output(GPIOA, AUX_ENABLE_PIN, 0UL);
    __DSB();
}

void board_safety_init(void)
{
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN |
                   RCC_AHB2ENR_GPIOCEN;
    (void)RCC_AHB2ENR;
    board_safety_force_off();

#if BOARD_FLT_INPUT_CONNECTED
    gpio_input_pulldown(GPIOA, FLT1_PIN);
    gpio_input_pulldown(GPIOA, FLT2_PIN);
    gpio_input_pulldown(GPIOB, FLT3_PIN);
    gpio_input_pulldown(GPIOB, FLT4_PIN);
    gpio_input_pulldown(GPIOB, FLT5_PIN);
    gpio_input_pulldown(GPIOC, FLT6_PIN);
    gpio_input_pulldown(GPIOC, EEV1_PIN);
    gpio_input_pulldown(GPIOC, EEV2_PIN);
#endif
}

uint32_t board_safety_read_fault_inputs(void)
{
#if !BOARD_FLT_INPUT_CONNECTED
    return 0UL;
#else
    uint32_t raw = 0UL;
    if (gpio_read(GPIOA, FLT1_PIN)) raw |= 1UL << 0;
    if (gpio_read(GPIOA, FLT2_PIN)) raw |= 1UL << 1;
    if (gpio_read(GPIOB, FLT3_PIN)) raw |= 1UL << 2;
    if (gpio_read(GPIOB, FLT4_PIN)) raw |= 1UL << 3;
    if (gpio_read(GPIOB, FLT5_PIN)) raw |= 1UL << 4;
    if (gpio_read(GPIOC, FLT6_PIN)) raw |= 1UL << 5;
    if (gpio_read(GPIOC, EEV1_PIN)) raw |= 1UL << 6;
    if (gpio_read(GPIOC, EEV2_PIN)) raw |= 1UL << 7;
    return raw;
#endif
}

uint32_t board_safety_outputs_are_off(void)
{
    if (board_safety_power_outputs_are_off() == 0UL) return 0UL;
    if (((GPIOA->ODR >> AUX_ENABLE_PIN) & 1UL) != 0UL) return 0UL;
    return 1UL;
}

uint32_t board_safety_power_outputs_are_off(void)
{
    board_hrtim_diag_t hrtim;
    board_hrtim_get_diag(&hrtim);
    if (((GPIOB->ODR >> OE1_PIN) & 1UL) == 0UL) return 0UL;
    if (((GPIOC->ODR >> OE2_PIN) & 1UL) == 0UL) return 0UL;
    if (((GPIOC->ODR >> OE3_PIN) & 1UL) == 0UL) return 0UL;
    if (((GPIOA->ODR >> GATE_INHIBIT_PIN) & 1UL) == 0UL) return 0UL;
    if ((hrtim.counters_enabled & ~HRTIM_MCR_TACEN) != 0UL ||
        ((hrtim.counters_enabled & HRTIM_MCR_TACEN) != 0UL &&
         hrtim.sampling_running == 0UL)) return 0UL;
    if (hrtim.pwm_pins_high_z == 0UL) return 0UL;
    return 1UL;
}

uint32_t board_safety_protection_ready(void)
{
#if BOARD_EXTERNAL_PROTECTION_CONFIRMED
    return 1UL;
#else
    return (BOARD_HARDWARE_FLT_CONFIRMED != 0U &&
            BOARD_V15_FEEDBACK_CONFIRMED != 0U &&
            BOARD_THERMAL_LIMITS_CONFIRMED != 0U &&
            BOARD_PV_LIMIT_CONFIRMED != 0U &&
            BOARD_CURRENT_POLARITY_CONFIRMED != 0U &&
            BOARD_CONTROL_GAINS_VALIDATED != 0U &&
            BOARD_V15_PGOOD_RUNTIME_IMPLEMENTED != 0U &&
            BOARD_FLT_RUNTIME_POLARITY_VALIDATED != 0U &&
            board_hrtim_fault_backend_ready() != 0UL) ? 1UL : 0UL;
#endif
}

uint32_t board_safety_request_power_on(void)
{
    uint32_t gate_fail_mask = 0UL;
    uint32_t raw_faults = board_safety_read_fault_inputs();
    if (BOARD_POWER_OUTPUT_ARMING_ENABLE == 0U) gate_fail_mask |= 1UL << 8;
    if (board_safety_protection_ready() == 0UL) gate_fail_mask |= 1UL << 9;
    if (raw_faults != 0UL) gate_fail_mask |= 1UL << 10;
    if (gate_fail_mask != 0UL) {
        board_hrtim_mark_arm_failure(1UL, gate_fail_mask | raw_faults);
        return 0UL;
    }

    /* Keep PWM blocked while requesting and verifying the auxiliary rail. */
    gpio_output(GPIOB, OE1_PIN, 1UL);
    gpio_output(GPIOC, OE2_PIN, 1UL);
    gpio_output(GPIOC, OE3_PIN, 1UL);
    gpio_output(GPIOA, GATE_INHIBIT_PIN, 1UL);
    gpio_output(GPIOA, AUX_ENABLE_PIN, 1UL);
    __DSB();
#if !BOARD_EXTERNAL_PROTECTION_CONFIRMED
    if (board_v15_pgood_read() == 0UL ||
        board_safety_read_fault_inputs() != 0UL ||
        board_hrtim_power_arm(1UL) == 0UL) {
#else
    if (board_hrtim_power_arm(1UL) == 0UL) {
#endif
        board_safety_force_off();
        return 0UL;
    }

    /* Only the non-synchronous CHE2/CHF1 bank is released. */
    gpio_output(GPIOC, OE3_PIN, 0UL);
    gpio_output(GPIOA, GATE_INHIBIT_PIN, 0UL);
    __DSB();
#if !BOARD_EXTERNAL_PROTECTION_CONFIRMED
    if (board_safety_read_fault_inputs() != 0UL || board_v15_pgood_read() == 0UL) {
#else
    if (board_safety_read_fault_inputs() != 0UL) {
#endif
        board_hrtim_mark_arm_failure(6UL, 1UL << 0);
        board_safety_force_off();
        return 0UL;
    }
    if (board_aux_get_request() == 0UL) gate_fail_mask |= 1UL << 1;
    if (((GPIOC->ODR >> OE3_PIN) & 1UL) != 0UL) gate_fail_mask |= 1UL << 2;
    if (((GPIOA->ODR >> GATE_INHIBIT_PIN) & 1UL) != 0UL) gate_fail_mask |= 1UL << 3;
    if (gpio_read(GPIOA, AUX_ENABLE_PIN) == 0UL) gate_fail_mask |= 1UL << 4;
    if (gpio_read(GPIOC, OE3_PIN) != 0UL) gate_fail_mask |= 1UL << 5;
    if (gpio_read(GPIOA, GATE_INHIBIT_PIN) != 0UL) gate_fail_mask |= 1UL << 6;
    if (board_hrtim_power_outputs_enabled() == 0UL) gate_fail_mask |= 1UL << 7;
    if (gate_fail_mask != 0UL) {
        board_hrtim_mark_arm_failure(6UL, gate_fail_mask);
        board_safety_force_off();
        return 0UL;
    }
    return 1UL;
}

void board_safety_get_diag(board_safety_diag_t *diag)
{
    if (diag == (board_safety_diag_t *)0) return;
    diag->raw_fault_inputs = board_safety_read_fault_inputs();
    diag->outputs_are_off = board_safety_outputs_are_off();
    diag->hardware_fault_routing_confirmed = BOARD_HARDWARE_FLT_CONFIRMED;
    diag->v15_feedback_confirmed = BOARD_V15_FEEDBACK_CONFIRMED;
    diag->v15_pgood_runtime = board_v15_pgood_read();
    diag->protection_backend_ready = board_safety_protection_ready();
}
