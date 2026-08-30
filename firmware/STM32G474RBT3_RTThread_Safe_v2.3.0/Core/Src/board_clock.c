#include "board_clock.h"
#include "board_config.h"
#include "stm32g474_bare.h"

#define CLOCK_TIMEOUT_LOOPS 1000000UL

static board_clock_diag_t clock_diag = {
    .source = BOARD_CLOCK_SOURCE_HSI16,
    .system_core_hz = BOARD_FALLBACK_SYSCLK_HZ,
    .fallback_active = 1UL,
    .error = BOARD_CLOCK_ERROR_NONE
};

static uint32_t wait_mask(volatile uint32_t *reg, uint32_t mask,
                          uint32_t expected)
{
    uint32_t timeout = CLOCK_TIMEOUT_LOOPS;
    while (((*reg & mask) != expected) && timeout-- != 0UL) {}
    return ((*reg & mask) == expected) ? 1UL : 0UL;
}

static uint32_t select_hsi16(void)
{
    RCC->CR |= RCC_CR_HSION;
    if (wait_mask(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY) == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_HSI_READY;
        return 0UL;
    }
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_SW_Msk | RCC_CFGR_HPRE_Msk |
                  RCC_CFGR_PPRE1_Msk | RCC_CFGR_PPRE2_Msk)) |
                  RCC_CFGR_SW_HSI;
    if (wait_mask(&RCC->CFGR, RCC_CFGR_SWS_Msk, RCC_CFGR_SWS_HSI) == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_HSI_SWITCH;
        return 0UL;
    }
    SystemCoreClock = BOARD_FALLBACK_SYSCLK_HZ;
    clock_diag.source = BOARD_CLOCK_SOURCE_HSI16;
    clock_diag.system_core_hz = BOARD_FALLBACK_SYSCLK_HZ;
    clock_diag.fallback_active = 1UL;
    return 1UL;
}

uint32_t board_clock_init_170mhz(void)
{
    uint32_t pllcfgr;

    clock_diag.error = BOARD_CLOCK_ERROR_NONE;
    clock_diag.hse_ready = 0UL;
    clock_diag.pll_ready = 0UL;
    clock_diag.range1_boost = 0UL;
    clock_diag.flash_latency = FLASH->ACR & FLASH_ACR_LATENCY_Msk;
    clock_diag.css_enabled = 0UL;
    clock_diag.fallback_active = 1UL;
    if (select_hsi16() == 0UL) return 0UL;

    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR5 &= ~PWR_CR5_R1MODE;
    PWR->CR1 = (PWR->CR1 & ~PWR_CR1_VOS_Msk) | PWR_CR1_VOS_0;
    if (wait_mask(&PWR->SR2, PWR_SR2_VOSF, 0UL) == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_VOS;
        return 0UL;
    }
    clock_diag.range1_boost =
        (((PWR->CR1 & PWR_CR1_VOS_Msk) == PWR_CR1_VOS_0) &&
         ((PWR->CR5 & PWR_CR5_R1MODE) == 0UL)) ? 1UL : 0UL;
    if (clock_diag.range1_boost == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_VOS;
        return 0UL;
    }

    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) |
                 FLASH_ACR_LATENCY_4WS;
    if ((FLASH->ACR & FLASH_ACR_LATENCY_Msk) != FLASH_ACR_LATENCY_4WS) {
        clock_diag.error = BOARD_CLOCK_ERROR_FLASH;
        return 0UL;
    }
    clock_diag.flash_latency = FLASH->ACR & FLASH_ACR_LATENCY_Msk;

    RCC->CR &= ~RCC_CR_HSEBYP;
    RCC->CR |= RCC_CR_HSEON;
    if (wait_mask(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY) == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_HSE_READY;
        return 0UL;
    }
    clock_diag.hse_ready = 1UL;

    /* A later HSE failure raises NMI. The strong NMI handler performs direct
     * HRTIM/OE/gate containment and lets the watchdog reset the MCU. */
    RCC->CICR = RCC_CICR_CSSC;
    RCC->CR |= RCC_CR_CSSON;
    clock_diag.css_enabled =
        ((RCC->CR & RCC_CR_CSSON) != 0UL) ? 1UL : 0UL;
    if (clock_diag.css_enabled == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_CSS_ENABLE;
        return 0UL;
    }

    RCC->CR &= ~RCC_CR_PLLON;
    if (wait_mask(&RCC->CR, RCC_CR_PLLRDY, 0UL) == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_PLL_DISABLE;
        return 0UL;
    }
    pllcfgr = RCC_PLLCFGR_PLLSRC_HSE |
              ((2UL - 1UL) << RCC_PLLCFGR_PLLM_Pos) |
              (85UL << RCC_PLLCFGR_PLLN_Pos) |
              RCC_PLLCFGR_PLLREN;
    RCC->PLLCFGR = pllcfgr;
    RCC->CR |= RCC_CR_PLLON;
    if (wait_mask(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY) == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_PLL_READY;
        return 0UL;
    }
    clock_diag.pll_ready = 1UL;

    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_SW_Msk | RCC_CFGR_HPRE_Msk |
                  RCC_CFGR_PPRE1_Msk | RCC_CFGR_PPRE2_Msk)) |
                  RCC_CFGR_SW_PLL;
    if (wait_mask(&RCC->CFGR, RCC_CFGR_SWS_Msk, RCC_CFGR_SWS_PLL) == 0UL) {
        clock_diag.error = BOARD_CLOCK_ERROR_PLL_SWITCH;
        (void)select_hsi16();
        return 0UL;
    }
    SystemCoreClock = BOARD_TARGET_SYSCLK_HZ;
    clock_diag.source = BOARD_CLOCK_SOURCE_HSE_PLL_170M;
    clock_diag.system_core_hz = BOARD_TARGET_SYSCLK_HZ;
    clock_diag.fallback_active = 0UL;
    clock_diag.error = BOARD_CLOCK_ERROR_NONE;
    return 1UL;
}

uint32_t board_clock_is_170mhz(void)
{
    const uint32_t expected_pll = RCC_PLLCFGR_PLLSRC_HSE |
        ((2UL - 1UL) << RCC_PLLCFGR_PLLM_Pos) |
        (85UL << RCC_PLLCFGR_PLLN_Pos) | RCC_PLLCFGR_PLLREN;
    const uint32_t pll_mask = RCC_PLLCFGR_PLLSRC_Msk |
        RCC_PLLCFGR_PLLM_Msk | RCC_PLLCFGR_PLLN_Msk |
        RCC_PLLCFGR_PLLR_Msk | RCC_PLLCFGR_PLLREN;
    return (clock_diag.source == BOARD_CLOCK_SOURCE_HSE_PLL_170M &&
            clock_diag.system_core_hz == BOARD_TARGET_SYSCLK_HZ &&
            (RCC->CFGR & RCC_CFGR_SWS_Msk) == RCC_CFGR_SWS_PLL &&
            (RCC->CR & (RCC_CR_HSERDY | RCC_CR_PLLRDY | RCC_CR_CSSON)) ==
                (RCC_CR_HSERDY | RCC_CR_PLLRDY | RCC_CR_CSSON) &&
            (RCC->PLLCFGR & pll_mask) == expected_pll &&
            (PWR->CR1 & PWR_CR1_VOS_Msk) == PWR_CR1_VOS_0 &&
            (PWR->CR5 & PWR_CR5_R1MODE) == 0UL &&
            (FLASH->ACR & FLASH_ACR_LATENCY_Msk) ==
                FLASH_ACR_LATENCY_4WS) ? 1UL : 0UL;
}

uint32_t board_clock_get_hz(void)
{
    return clock_diag.system_core_hz;
}

void board_clock_get_diag(board_clock_diag_t *diag)
{
    if (diag != (board_clock_diag_t *)0) *diag = clock_diag;
}
