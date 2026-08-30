#ifndef STM32G474_BARE_H
#define STM32G474_BARE_H

/*
 * Compatibility layer for the validated register-level firmware.
 *
 * Peripheral structures, base addresses, interrupt numbers and bit fields now
 * come from ST's official CMSIS device header. The short lvalue aliases below
 * preserve the v1.8.1 application source while removing the hand-written
 * register map that previously had to be audited field by field.
 */
#include "stm32g4xx.h"

#include <stddef.h>
#include <stdint.h>

#define REG32(address) (*(volatile uint32_t *)(address))

#define RCC_CR          (RCC->CR)
#define RCC_CFGR        (RCC->CFGR)
#define RCC_CCIPR       (RCC->CCIPR)
#define RCC_CSR         (RCC->CSR)
#define RCC_AHB2RSTR    (RCC->AHB2RSTR)
#define RCC_AHB2ENR     (RCC->AHB2ENR)
#define RCC_APB2ENR     (RCC->APB2ENR)
#define RCC_APB2RSTR    (RCC->APB2RSTR)

#define IWDG_KR         (IWDG->KR)
#define IWDG_PR         (IWDG->PR)
#define IWDG_RLR        (IWDG->RLR)
#define IWDG_SR         (IWDG->SR)

#define RCC_CFGR_SW_MASK       RCC_CFGR_SW_Msk
#define RCC_CFGR_SW_HSI16      RCC_CFGR_SW_HSI
#define RCC_CFGR_SWS_MASK      RCC_CFGR_SWS_Msk
#define RCC_CFGR_SWS_HSI16     RCC_CFGR_SWS_HSI
#define RCC_CFGR_HPRE_MASK     RCC_CFGR_HPRE_Msk
#define RCC_CFGR_PPRE1_MASK    RCC_CFGR_PPRE1_Msk
#define RCC_CFGR_PPRE2_MASK    RCC_CFGR_PPRE2_Msk

#define USART_ICR_ERROR_MASK \
    (USART_ICR_PECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_ORECF)

#define HRTIM_COMMON_ODISR (HRTIM1->sCommonRegs.ODISR)
#define HRTIM_OUTPUT_ALL \
    (HRTIM_ODISR_TA1ODIS | HRTIM_ODISR_TA2ODIS | \
     HRTIM_ODISR_TB1ODIS | HRTIM_ODISR_TB2ODIS | \
     HRTIM_ODISR_TC1ODIS | HRTIM_ODISR_TC2ODIS | \
     HRTIM_ODISR_TD1ODIS | HRTIM_ODISR_TD2ODIS | \
     HRTIM_ODISR_TE1ODIS | HRTIM_ODISR_TE2ODIS | \
     HRTIM_ODISR_TF1ODIS | HRTIM_ODISR_TF2ODIS)

#define SCB_CSR          (SCB->SHCSR)
#define COREDEBUG_DEMCR  (CoreDebug->DEMCR)
#define DWT_CTRL         (DWT->CTRL)
#define DWT_CYCCNT       (DWT->CYCCNT)

/* Preserve the v1.8.1 register-offset regression checks, now against the
 * official CMSIS structures used by the build. */
_Static_assert(offsetof(ADC_TypeDef, TR1) == 0x20U, "ADC TR1 offset");
_Static_assert(offsetof(ADC_TypeDef, SQR1) == 0x30U, "ADC SQR1 offset");
_Static_assert(offsetof(ADC_TypeDef, DR) == 0x40U, "ADC DR offset");
_Static_assert(offsetof(ADC_TypeDef, DIFSEL) == 0xB0U, "ADC DIFSEL offset");
_Static_assert(offsetof(ADC_TypeDef, CALFACT) == 0xB4U, "ADC CALFACT offset");
_Static_assert(offsetof(USART_TypeDef, ISR) == 0x1CU, "USART ISR offset");
_Static_assert(offsetof(USART_TypeDef, RDR) == 0x24U, "USART RDR offset");
_Static_assert(offsetof(USART_TypeDef, TDR) == 0x28U, "USART TDR offset");
_Static_assert(offsetof(HRTIM_TypeDef, sCommonRegs) +
               offsetof(HRTIM_Common_TypeDef, ODISR) == 0x398U,
               "HRTIM ODISR offset");

#endif
