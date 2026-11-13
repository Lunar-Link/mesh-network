/* STM32F446RE specifics behind board.h and the storage log; platformio.ini puts this folder on the include path. */
#ifndef MCU_H
#define MCU_H

#include "stm32f4xx_hal.h"

#define MCU_NAME "STM32F446RE"

/* 84 MHz core: APB2 (SPI1) at 84 MHz, APB1 (SPI2) at 42 MHz; both SPIs at 5.25 MHz */
#define RADIO_SPI_PRESCALER SPI_BAUDRATEPRESCALER_16
#define AUX_SPI_PRESCALER SPI_BAUDRATEPRESCALER_8

/* PC0 (A5) is ADC123_IN10 on the F4 */
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_10

/* Storage log: flash sectors 5 to 7, 128 KB each, the top 384 KB of the 512 KB. The F4 erases whole sectors, so a log
   page is one sector. platformio.ini caps the program at sectors 0 to 4 (128 KB) below it. */
#define STORAGE_ADDR 0x08020000u
#define STORAGE_PAGES 3u
#define STORAGE_PAGE 0x20000u

#endif
