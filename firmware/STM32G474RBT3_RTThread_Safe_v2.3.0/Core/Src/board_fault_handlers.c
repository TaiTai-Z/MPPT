#include "board_config.h"
#include "board_hrtim.h"
#include "stm32g474_bare.h"

/* Strong definitions override the weak CMSIS startup handlers. The sequence
 * uses only direct register writes so a scheduler, heap and C library are not
 * required. The watchdog is deliberately not fed; after containment it resets
 * the MCU into the same safe boot path. */
static __attribute__((noreturn)) void fatal_shutdown(void)
{
    __disable_irq();
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN |
                    RCC_AHB2ENR_GPIOBEN |
                    RCC_AHB2ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_HRTIM1EN;
    (void)RCC->APB2ENR;

    board_hrtim_force_off();
    GPIOB->BSRR = 1UL << OE1_PIN;
    GPIOC->BSRR = (1UL << OE2_PIN) | (1UL << OE3_PIN);
    GPIOA->BSRR = (1UL << GATE_INHIBIT_PIN) |
                  (1UL << (AUX_ENABLE_PIN + 16U));

    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (OE1_PIN * 2U))) |
                   (1UL << (OE1_PIN * 2U));
    GPIOC->MODER = (GPIOC->MODER &
                   ~((3UL << (OE2_PIN * 2U)) |
                     (3UL << (OE3_PIN * 2U)))) |
                   (1UL << (OE2_PIN * 2U)) |
                   (1UL << (OE3_PIN * 2U));
    GPIOA->MODER = (GPIOA->MODER &
                   ~((3UL << (GATE_INHIBIT_PIN * 2U)) |
                     (3UL << (AUX_ENABLE_PIN * 2U)))) |
                   (1UL << (GATE_INHIBIT_PIN * 2U)) |
                   (1UL << (AUX_ENABLE_PIN * 2U));

    /* Start a bounded recovery reset even if the exception occurred before
     * normal watchdog initialization. */
    IWDG->KR = 0x5555UL;
    IWDG->PR = 3UL;
    IWDG->RLR = 1999UL;
    IWDG->KR = 0xCCCCUL;
    __DSB();

    for (;;) {
        __NOP();
    }
}

static __attribute__((noreturn)) void hrtim_irq_shutdown(void)
{
    /* First effective operation is the peripheral output-disable strobe.
     * GPIO containment follows before entering the common watchdog reset. */
    HRTIM1->sCommonRegs.ODISR = HRTIM_OUTPUT_ALL;
    GPIOB->BSRR = 1UL << OE1_PIN;
    GPIOC->BSRR = (1UL << OE2_PIN) | (1UL << OE3_PIN);
    GPIOA->BSRR = (1UL << GATE_INHIBIT_PIN) |
                  (1UL << (AUX_ENABLE_PIN + 16U));
    __DSB();
    fatal_shutdown();
}

void NMI_Handler(void)        { fatal_shutdown(); }
#if !defined(APP_USE_RTTHREAD)
void HardFault_Handler(void)  { fatal_shutdown(); }
#endif
void MemManage_Handler(void)  { fatal_shutdown(); }
void BusFault_Handler(void)   { fatal_shutdown(); }
void UsageFault_Handler(void) { fatal_shutdown(); }
void HRTIM1_Master_IRQHandler(void) { hrtim_irq_shutdown(); }
void HRTIM1_TIMA_IRQHandler(void)   { hrtim_irq_shutdown(); }
void HRTIM1_TIMB_IRQHandler(void)   { hrtim_irq_shutdown(); }
void HRTIM1_TIMC_IRQHandler(void)   { hrtim_irq_shutdown(); }
void HRTIM1_TIMD_IRQHandler(void)   { hrtim_irq_shutdown(); }
void HRTIM1_TIME_IRQHandler(void)   { hrtim_irq_shutdown(); }
void HRTIM1_FLT_IRQHandler(void)    { hrtim_irq_shutdown(); }
void HRTIM1_TIMF_IRQHandler(void)   { hrtim_irq_shutdown(); }
