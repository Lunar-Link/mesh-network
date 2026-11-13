/* Interrupt-driven UART byte streams; the HAL completion callbacks for every UART land here. */
#include "serial.h"

#include <string.h>

#define MAX_STREAMS 2

static serial *streams[MAX_STREAMS];

static serial *find(UART_HandleTypeDef *h)
{
    for (size_t i = 0; i < MAX_STREAMS; i++) {
        if (streams[i] && streams[i]->uart == h) {
            return streams[i];
        }
    }
    return NULL;
}

static void arm_rx(serial *s)
{
    if (s->uart->RxState == HAL_UART_STATE_READY) {
        HAL_UART_Receive_IT(s->uart, &s->rx_byte, 1);
    }
}

/* Starts the next contiguous chunk; called with interrupts off or from the UART interrupt. */
static void start_tx(serial *s)
{
    uint32_t pending = s->tx_head - s->tx_tail;
    if (pending == 0) {
        s->tx_active = false;
        return;
    }
    uint32_t at = s->tx_tail & s->tx_mask;
    uint32_t chunk = s->tx_mask + 1u - at;
    if (chunk > pending) {
        chunk = pending;
    }
    s->tx_len = chunk;
    s->tx_active = true;
    if (HAL_UART_Transmit_IT(s->uart, &s->tx_buf[at], (uint16_t)chunk) != HAL_OK) {
        s->tx_active = false;
    }
}

void serial_init(serial *s, UART_HandleTypeDef *uart, uint8_t *rx, size_t rx_size, uint8_t *tx, size_t tx_size)
{
    memset(s, 0, sizeof *s);
    s->uart = uart;
    s->rx_buf = rx;
    s->tx_buf = tx;
    s->rx_mask = (uint32_t)rx_size - 1u;
    s->tx_mask = (uint32_t)tx_size - 1u;
    for (size_t i = 0; i < MAX_STREAMS; i++) {
        if (!streams[i] || streams[i]->uart == uart) {
            streams[i] = s;
            break;
        }
    }
    arm_rx(s);
}

int serial_getc(serial *s)
{
    if (s->rx_head == s->rx_tail) {
        return -1;
    }
    uint8_t c = s->rx_buf[s->rx_tail & s->rx_mask];
    s->rx_tail++;
    return c;
}

size_t serial_tx_free(const serial *s)
{
    return (size_t)(s->tx_mask + 1u - (s->tx_head - s->tx_tail));
}

bool serial_write(serial *s, const void *data, size_t len)
{
    if (len > serial_tx_free(s)) {
        s->tx_dropped++;
        return false;
    }
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        s->tx_buf[(s->tx_head + i) & s->tx_mask] = p[i];
    }
    __disable_irq();
    s->tx_head += (uint32_t)len;
    if (!s->tx_active) {
        start_tx(s);
    }
    __enable_irq();
    return true;
}

bool serial_tx_idle(const serial *s)
{
    return !s->tx_active;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h)
{
    serial *s = find(h);
    if (!s) {
        return;
    }
    if (s->rx_head - s->rx_tail <= s->rx_mask) {
        s->rx_buf[s->rx_head & s->rx_mask] = s->rx_byte;
        s->rx_head++;
    } else {
        s->rx_overruns++;
    }
    arm_rx(s);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *h)
{
    serial *s = find(h);
    if (!s) {
        return;
    }
    s->tx_tail += s->tx_len;
    start_tx(s);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *h)
{
    serial *s = find(h);
    if (!s) {
        return;
    }
    s->rx_overruns++;
    arm_rx(s);
    if (s->tx_active && h->gState == HAL_UART_STATE_READY) {
        start_tx(s);
    }
}
