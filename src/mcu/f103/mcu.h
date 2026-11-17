/* STM32F103C8 (Blue Pill) specifics behind board.h and the storage log; platformio.ini puts this folder on the include
   path. With 20 KB of RAM and 64 KB of flash, this chip also runs smaller library tables, set in platformio.ini. */
#ifndef MCU_H
#define MCU_H

#include "stm32f1xx_hal.h"

#include "pins_bluepill.h"

#define MCU_NAME "STM32F103C8"

/* F1 pins have no alternate-function number: a pin a peripheral drives is put in alternate mode, one it only reads is
   a plain input */
#define MCU_AF_INPUT GPIO_MODE_AF_INPUT
#define MCU_SET_AF(init, af) ((void)0)

/* 64 MHz core: APB2 (SPI1) at 64 MHz, APB1 (SPI2) at 32 MHz; both SPIs at 4 MHz */
#define RADIO_SPI_PRESCALER SPI_BAUDRATEPRESCALER_16
#define AUX_SPI_PRESCALER SPI_BAUDRATEPRESCALER_8

/* PA0 is ADC12_IN0 */
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0

/* Storage log: the top 8 KB of the 64 KB of flash as 4 log pages. The F103 erases 1 KB pages, and a log page must
   hold a copy of every slot, so each is two of them. platformio.ini caps the program below it. */
#define STORAGE_ADDR 0x0800E000u
#define STORAGE_PAGES 4u
#define STORAGE_PAGE 2048u

/* Console transmit ring; the default 8 KB ring and its reply buffer would take three quarters of the RAM */
#define CONSOLE_TX_SIZE 2048

#endif
