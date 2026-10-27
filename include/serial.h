/* Interrupt-driven byte streams over a HAL UART: receive and transmit rings, never blocking. */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

typedef struct serial {
    UART_HandleTypeDef *uart;
    uint8_t *rx_buf;
    uint8_t *tx_buf;
    uint32_t rx_mask;
    uint32_t tx_mask;
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    volatile uint32_t tx_len;
    volatile bool tx_active;
    uint8_t rx_byte;
    volatile uint32_t rx_overruns;
    uint32_t tx_dropped;
} serial;

/* Ring sizes must be powers of two. */
void serial_init(serial *s, UART_HandleTypeDef *uart, uint8_t *rx, size_t rx_size, uint8_t *tx, size_t tx_size);
int serial_getc(serial *s);
size_t serial_tx_free(const serial *s);
/* Queues all of data or none of it; false (and a drop counted) when it does not fit. */
bool serial_write(serial *s, const void *data, size_t len);
bool serial_tx_idle(const serial *s);

#endif
