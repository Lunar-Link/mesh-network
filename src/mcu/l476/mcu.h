/* STM32L476RG specifics behind board.h and the storage log; platformio.ini puts this folder on the include path. */
#ifndef MCU_H
#define MCU_H

#include "stm32l4xx_hal.h"

#define MCU_NAME "STM32L476RG"

/* 80 MHz core with APB1 and APB2 undivided; both SPIs at 5 MHz */
#define RADIO_SPI_PRESCALER SPI_BAUDRATEPRESCALER_16
#define AUX_SPI_PRESCALER SPI_BAUDRATEPRESCALER_16

/* PC0 (A5) is ADC123_IN1 on the L4 */
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_1

/* Storage log: the top 128 KB of flash bank 2 as 64 pages of 2 KB. platformio.ini caps the program below it. */
#define STORAGE_ADDR 0x080E0000u
#define STORAGE_PAGES 64u
#define STORAGE_PAGE 2048u

#endif
