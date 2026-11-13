/* Host stand-in for board.h and the STM32 HAL, so the RYLR498 driver compiles against the fake module in test_rylr498.c. */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int UART_HandleTypeDef;

extern UART_HandleTypeDef board_radio_uart;

#define RADIO_UART_BAUD 115200u

uint32_t HAL_GetTick(void);
void board_radio_reset(bool asserted);

#endif
