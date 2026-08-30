#include "board_config.h"
#include "board_hrtim.h"
#include "board_safety.h"
#include "board_clock.h"
#include "board_uart.h"
#include "stm32g474_bare.h"
#include <rthw.h>
#include <rtthread.h>

#define RT_UART_TX_TIMEOUT 100000UL

static void rt_early_uart_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->CCIPR = (RCC->CCIPR & ~3UL) | 2UL;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;
    GPIOB->AFR[0] = (GPIOB->AFR[0] &
                    ~((0xFUL << 24) | (0xFUL << 28))) |
                    (7UL << 24) | (7UL << 28);
    GPIOB->MODER = (GPIOB->MODER &
                   ~((3UL << 12) | (3UL << 14))) |
                   (2UL << 12) | (2UL << 14);
    GPIOB->PUPDR = (GPIOB->PUPDR &
                   ~((3UL << 12) | (3UL << 14))) |
                   (1UL << 14);
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR &
                     ~((3UL << 12) | (3UL << 14))) |
                     (2UL << 12) | (2UL << 14);
    USART1->CR1 = 0UL;
    USART1->CR2 = 0UL;
    USART1->CR3 = 0UL;
    USART1->PRESC = 0UL;
    USART1->ICR = 0xFFFFFFFFUL;
    USART1->BRR = BOARD_UART_BRR_HSI16_115200;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static rt_err_t rt_safety_exception_hook(void *context)
{
    (void)context;
    board_safety_force_off();
    return -RT_ERROR;
}

void rt_hw_board_init(void)
{
    SystemCoreClockUpdate();
    board_hrtim_safe_init();
    board_safety_init();
    (void)board_clock_init_170mhz();
    board_hrtim_safe_init();
    board_safety_force_off();
    rt_early_uart_init();
    rt_hw_exception_install(rt_safety_exception_hook);

    if (SysTick_Config(board_clock_get_hz() / RT_TICK_PER_SECOND) != 0UL) {
        board_safety_force_off();
        for (;;) { __NOP(); }
    }
}

void SysTick_Handler(void)
{
    rt_interrupt_enter();
    rt_tick_increase();
    rt_interrupt_leave();
}

void rt_hw_console_output(const char *text)
{
    if (text == RT_NULL) return;
    /* Once the application UART is ready, RT-Thread logging must share the
     * bounded TX ring.  Waiting on TXE here would block the priority-6 main
     * owner and can delay the 10-ms outer loop. */
    if (board_uart_is_initialized() != 0UL) {
        while (*text != '\0') {
            if (*text == '\n') (void)board_uart_put_byte('\r');
            (void)board_uart_put_byte((uint8_t)*text++);
        }
        return;
    }
    while (*text != '\0') {
        uint32_t timeout = RT_UART_TX_TIMEOUT;
        if (*text == '\n') {
            while ((USART1->ISR & USART_ISR_TXE) == 0UL && timeout-- != 0UL) {}
            if ((USART1->ISR & USART_ISR_TXE) != 0UL) USART1->TDR = '\r';
            timeout = RT_UART_TX_TIMEOUT;
        }
        while ((USART1->ISR & USART_ISR_TXE) == 0UL && timeout-- != 0UL) {}
        if ((USART1->ISR & USART_ISR_TXE) == 0UL) break;
        USART1->TDR = (uint32_t)(uint8_t)*text++;
    }
}

void rt_hw_cpu_reset(void)
{
    board_safety_force_off();
    NVIC_SystemReset();
}

void rt_hw_cpu_shutdown(void)
{
    board_safety_force_off();
    for (;;) { __WFI(); }
}
