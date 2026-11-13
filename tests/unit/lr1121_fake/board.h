/* Host stand-in for board.h and the STM32 HAL, so the LR1121 driver compiles against the fake chip in test_lr1121.c. */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int GPIO_TypeDef;
typedef int SPI_HandleTypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;

extern GPIO_TypeDef fake_port;
extern SPI_HandleTypeDef board_radio_spi;

#define RADIO_NSS_PORT (&fake_port)
#define RADIO_NSS_PIN 1u
#define RADIO_DIO0_PORT (&fake_port)
#define RADIO_DIO0_PIN 2u
#define RADIO_BUSY_PORT (&fake_port)
#define RADIO_BUSY_PIN 4u

uint32_t HAL_GetTick(void);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t timeout);
void board_radio_reset(bool asserted);

#endif
