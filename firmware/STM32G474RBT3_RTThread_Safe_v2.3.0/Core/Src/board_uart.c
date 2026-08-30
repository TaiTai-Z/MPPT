#include "board_uart.h"
#include "board_config.h"
#include "stm32g474_bare.h"

#define UART_RX_CAPACITY 256UL
#define UART_TX_CAPACITY 4096UL

static uint8_t rx_buffer[UART_RX_CAPACITY];
static uint8_t tx_buffer[UART_TX_CAPACITY];
static volatile uint32_t rx_head, rx_tail, tx_head, tx_tail;
static volatile uint32_t rx_drop, tx_drop, tx_frame_drop, error_count, irq_count;
static volatile uint32_t initialized;

_Static_assert((UART_RX_CAPACITY & (UART_RX_CAPACITY - 1UL)) == 0UL,
               "UART RX capacity must be a power of two");
_Static_assert((UART_TX_CAPACITY & (UART_TX_CAPACITY - 1UL)) == 0UL,
               "UART TX capacity must be a power of two");

static void capture_rx(void)
{
    uint32_t budget = UART_RX_CAPACITY;
    while (budget-- != 0UL) {
        uint32_t status = USART1->ISR;
        if ((status & (USART_ISR_PE | USART_ISR_FE | USART_ISR_NE |
                       USART_ISR_ORE)) != 0UL) {
            USART1->ICR = status & USART_ICR_ERROR_MASK;
            ++error_count;
        }
        if ((status & USART_ISR_RXNE) != 0UL) {
            uint32_t next;
            uint8_t value = (uint8_t)(USART1->RDR & 0xFFUL);
            next = (rx_head + 1UL) & (UART_RX_CAPACITY - 1UL);
            if (next == rx_tail) {
                ++rx_drop;
            } else {
                rx_buffer[rx_head] = value;
                __DMB();
                rx_head = next;
            }
        }
        if ((USART1->ISR & (USART_ISR_RXNE | USART_ISR_PE | USART_ISR_FE |
                            USART_ISR_NE | USART_ISR_ORE)) == 0UL) break;
    }
}

void board_uart_init(void)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    initialized = 0UL;
    rx_head = 0UL; rx_tail = 0UL; tx_head = 0UL; tx_tail = 0UL;
    rx_drop = 0UL; tx_drop = 0UL; tx_frame_drop = 0UL;
    error_count = 0UL; irq_count = 0UL;

    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC_CCIPR = (RCC_CCIPR & ~3UL) | 2UL; /* USART1SEL=HSI16. */
    RCC_APB2ENR |= RCC_APB2ENR_USART1EN;
    RCC_APB2RSTR |= RCC_APB2RSTR_USART1RST;
    RCC_APB2RSTR &= ~RCC_APB2RSTR_USART1RST;

    GPIOB->AFR[0] = (GPIOB->AFR[0] &
                    ~((0xFUL << 24) | (0xFUL << 28))) |
                    (7UL << 24) | (7UL << 28);
    GPIOB->MODER = (GPIOB->MODER &
                    ~((3UL << 12) | (3UL << 14))) |
                    (2UL << 12) | (2UL << 14);
    GPIOB->PUPDR = (GPIOB->PUPDR &
                    ~((3UL << 12) | (3UL << 14))) | (1UL << 14);
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR &
                      ~((3UL << 12) | (3UL << 14))) |
                      (2UL << 12) | (2UL << 14);

    USART1->CR1 = 0UL;
    USART1->CR2 = 0UL;
    USART1->CR3 = 0UL;
    USART1->PRESC = 0UL;
    USART1->ICR = 0xFFFFFFFFUL;
    USART1->BRR = BOARD_UART_BRR_HSI16_115200;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE |
                  USART_CR1_RXNEIE;

    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_SetPriority(USART1_IRQn, 5U);
    initialized = 1UL;
    NVIC_EnableIRQ(USART1_IRQn);
    if (state == 0UL) __enable_irq();
}

uint32_t board_uart_put_byte(uint8_t value)
{
    uint32_t next;
    uint32_t state;
    if (initialized == 0UL) return 0UL;
    state = __get_PRIMASK();
    __disable_irq();
    next = (tx_head + 1UL) & (UART_TX_CAPACITY - 1UL);
    if (next == tx_tail) {
        ++tx_drop;
        if (state == 0UL) __enable_irq();
        return 0UL;
    }
    tx_buffer[tx_head] = value;
    __DMB();
    tx_head = next;
    USART1->CR1 |= USART_CR1_TXEIE;
    if (state == 0UL) __enable_irq();
    return 1UL;
}

uint32_t board_uart_write(const char *data, uint32_t length)
{
    uint32_t written = 0UL;
    if (data == (const char *)0) return 0UL;
    while (written < length && board_uart_put_byte((uint8_t)data[written]) != 0UL)
        ++written;
    return written;
}

uint32_t board_uart_write_atomic(const char *data, uint32_t length)
{
    uint32_t state, used, free_bytes, i;
    if (initialized == 0UL || data == (const char *)0 || length == 0UL ||
        length >= UART_TX_CAPACITY) return 0UL;
    state = __get_PRIMASK();
    __disable_irq();
    used = (tx_head - tx_tail) & (UART_TX_CAPACITY - 1UL);
    free_bytes = (UART_TX_CAPACITY - 1UL) - used;
    if (length > free_bytes) {
        ++tx_frame_drop;
        tx_drop += length;
        if (state == 0UL) __enable_irq();
        return 0UL;
    }
    for (i = 0UL; i < length; ++i) {
        tx_buffer[tx_head] = (uint8_t)data[i];
        tx_head = (tx_head + 1UL) & (UART_TX_CAPACITY - 1UL);
    }
    __DMB();
    USART1->CR1 |= USART_CR1_TXEIE;
    if (state == 0UL) __enable_irq();
    return length;
}

uint32_t board_uart_get_byte(uint8_t *value)
{
    if (value == (uint8_t *)0 || rx_tail == rx_head) return 0UL;
    __DMB();
    *value = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1UL) & (UART_RX_CAPACITY - 1UL);
    return 1UL;
}

void board_uart_irq_service(void)
{
    ++irq_count;
    capture_rx();
    if ((USART1->ISR & USART_ISR_TXE) != 0UL &&
        (USART1->CR1 & USART_CR1_TXEIE) != 0UL) {
        if (tx_tail != tx_head) {
            __DMB();
            USART1->TDR = tx_buffer[tx_tail];
            tx_tail = (tx_tail + 1UL) & (UART_TX_CAPACITY - 1UL);
        } else {
            USART1->CR1 &= ~USART_CR1_TXEIE;
        }
    }
}

uint32_t board_uart_is_initialized(void)
{
    return initialized;
}

void board_uart_get_diag(board_uart_diag_t *diag)
{
    if (diag == (board_uart_diag_t *)0) return;
    diag->initialized = initialized;
    diag->rx_queued = (rx_head - rx_tail) & (UART_RX_CAPACITY - 1UL);
    diag->tx_queued = (tx_head - tx_tail) & (UART_TX_CAPACITY - 1UL);
    diag->rx_drop = rx_drop;
    diag->tx_drop = tx_drop;
    diag->tx_frame_drop = tx_frame_drop;
    diag->error_count = error_count;
    diag->irq_count = irq_count;
    diag->brr = USART1->BRR;
    diag->presc = USART1->PRESC;
}
