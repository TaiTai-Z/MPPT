#ifndef BOARD_UART_H
#define BOARD_UART_H

#include <stdint.h>

typedef struct {
    uint32_t initialized;
    uint32_t rx_queued;
    uint32_t tx_queued;
    uint32_t rx_drop;
    uint32_t tx_drop;
    uint32_t tx_frame_drop;
    uint32_t error_count;
    uint32_t irq_count;
    uint32_t brr;
    uint32_t presc;
} board_uart_diag_t;

/* USART1 on PB6/PB7.  Both RX and TX are interrupt backed.  Writers never
 * wait for the wire; a full queue is reported as a drop instead. */
void board_uart_init(void);
void board_uart_irq_service(void);
uint32_t board_uart_put_byte(uint8_t value);
uint32_t board_uart_write(const char *data, uint32_t length);
/* Queues the complete frame or queues nothing. */
uint32_t board_uart_write_atomic(const char *data, uint32_t length);
uint32_t board_uart_get_byte(uint8_t *value);
uint32_t board_uart_is_initialized(void);
void board_uart_get_diag(board_uart_diag_t *diag);

#endif
