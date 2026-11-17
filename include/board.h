/* Peripheral handles and board functions for the node board: a Nucleo-F446RE, a Nucleo-L476RG or a Blue Pill
   (STM32F103C8). The pin map is per board, in pins_nucleo64.h or pins_bluepill.h, and what differs per chip (clock,
   I2C, ADC, random numbers, flash) is in src/mcu/<chip>/; the chip's mcu.h brings in both. */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>

#include "mcu.h"

/* Battery divider and ADC reference, the same on every board */
#define BATTERY_DIVIDER_NUM 2u /* battery volts = pin volts * NUM / DEN */
#define BATTERY_DIVIDER_DEN 1u
#define ADC_VREF_MV 3300u

extern SPI_HandleTypeDef board_radio_spi;
extern SPI_HandleTypeDef board_aux_spi;
extern I2C_HandleTypeDef board_sensor_i2c;
extern UART_HandleTypeDef board_host_uart;
extern UART_HandleTypeDef board_radio_uart;
extern ADC_HandleTypeDef board_adc;

void board_init(void);
uint16_t board_battery_mv(void);
void board_radio_reset(bool asserted);
/* Halts with interrupts off; for setup that cannot go on. */
void board_fail(void);

/* Provided by src/mcu/<chip>/mcu.c */
void mcu_clock_init(void);
void mcu_i2c_init(void);
void mcu_adc_init(void);
/* One 12-bit conversion of an ADC1 channel; false if the ADC did not finish. */
bool mcu_adc_read(uint32_t channel, uint32_t *raw);
/* A seed for the port's random numbers, different on each start: the L476's hardware RNG, or on the F446 and F103,
   which have no RNG, the device id mixed with ADC noise. */
uint32_t board_entropy(void);

#endif
