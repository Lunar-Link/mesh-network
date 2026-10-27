/* Pin map and peripheral handles for the Nucleo-L476RG node board. */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>

#include "stm32l4xx_hal.h"

/* Radio on SPI1 with the pins most LoRa shields use on the Arduino header */
#define RADIO_SPI SPI1
#define RADIO_SPI_PRESCALER SPI_BAUDRATEPRESCALER_16 /* 5 MHz at 80 MHz */
#define RADIO_SCK_PORT GPIOA
#define RADIO_SCK_PIN GPIO_PIN_5 /* D13, shared with the LD2 LED, which is therefore unused */
#define RADIO_MISO_PORT GPIOA
#define RADIO_MISO_PIN GPIO_PIN_6 /* D12 */
#define RADIO_MOSI_PORT GPIOA
#define RADIO_MOSI_PIN GPIO_PIN_7 /* D11 */
#define RADIO_NSS_PORT GPIOB
#define RADIO_NSS_PIN GPIO_PIN_6 /* D10 */
#define RADIO_RESET_PORT GPIOC
#define RADIO_RESET_PIN GPIO_PIN_7 /* D9 */
#define RADIO_DIO0_PORT GPIOA
#define RADIO_DIO0_PIN GPIO_PIN_10 /* D2, rising edge on transmit done and receive done */
#define RADIO_DIO0_IRQN EXTI15_10_IRQn

/* UART radios (AT-command modules) on UART4 at the Arduino A0/A1 pins; they share the reset line above */
#define RADIO_UART UART4
#define RADIO_UART_IRQN UART4_IRQn
#define RADIO_UART_BAUD 115200u
#define RADIO_UART_TX_PORT GPIOA
#define RADIO_UART_TX_PIN GPIO_PIN_0 /* A0, to the module's RXD */
#define RADIO_UART_RX_PORT GPIOA
#define RADIO_UART_RX_PIN GPIO_PIN_1 /* A1, from the module's TXD */

/* Second SPI on the Morpho headers for storage or a second radio */
#define AUX_SPI SPI2
#define AUX_SPI_PRESCALER SPI_BAUDRATEPRESCALER_16
#define AUX_SCK_PORT GPIOB
#define AUX_SCK_PIN GPIO_PIN_13 /* CN10 pin 30 */
#define AUX_MISO_PORT GPIOC
#define AUX_MISO_PIN GPIO_PIN_2 /* CN7 pin 35 */
#define AUX_MOSI_PORT GPIOC
#define AUX_MOSI_PIN GPIO_PIN_3 /* CN7 pin 37 */
#define AUX_NSS_PORT GPIOB
#define AUX_NSS_PIN GPIO_PIN_12 /* CN10 pin 16 */

/* Sensor bus on I2C1 at the Arduino D14/D15 pins, 100 kHz */
#define SENSOR_I2C I2C1
#define SENSOR_I2C_TIMING 0x10909CECu
#define SENSOR_SCL_PORT GPIOB
#define SENSOR_SCL_PIN GPIO_PIN_8 /* D15 */
#define SENSOR_SDA_PORT GPIOB
#define SENSOR_SDA_PIN GPIO_PIN_9 /* D14 */

/* Line protocol on USART2, which the ST-LINK presents to the laptop as a virtual COM port */
#define HOST_UART USART2
#define HOST_UART_IRQN USART2_IRQn
#define HOST_UART_BAUD 115200u
#define HOST_TX_PORT GPIOA
#define HOST_TX_PIN GPIO_PIN_2
#define HOST_RX_PORT GPIOA
#define HOST_RX_PIN GPIO_PIN_3

/* Battery sense on ADC1 channel 1 through a resistor divider */
#define BATTERY_PORT GPIOC
#define BATTERY_PIN GPIO_PIN_0 /* A5 */
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_1
#define BATTERY_DIVIDER_NUM 2u /* battery volts = pin volts * NUM / DEN */
#define BATTERY_DIVIDER_DEN 1u
#define ADC_VREF_MV 3300u

/* User button B1 */
#define BUTTON_PORT GPIOC
#define BUTTON_PIN GPIO_PIN_13

extern SPI_HandleTypeDef board_radio_spi;
extern SPI_HandleTypeDef board_aux_spi;
extern I2C_HandleTypeDef board_sensor_i2c;
extern UART_HandleTypeDef board_host_uart;
extern UART_HandleTypeDef board_radio_uart;
extern ADC_HandleTypeDef board_adc;
extern RNG_HandleTypeDef board_rng;

void board_init(void);
uint16_t board_battery_mv(void);
void board_radio_reset(bool asserted);

#endif
