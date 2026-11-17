/* Pin map for the Blue Pill (STM32F103C8T6, 48 pins). It keeps off the pins the board commits elsewhere: PA11/PA12
   (USB), PA13/PA14 (SWD), PA15/PB3/PB4 (JTAG until remapped), PB2 (BOOT1), PC13 (LED) and PC14/PC15 (32 kHz crystal).
   The F1 has no alternate-function numbers, so there are no _AF names here. Included through the chip's mcu.h. */
#ifndef PINS_BLUEPILL_H
#define PINS_BLUEPILL_H

/* Radio on SPI1 at its default pins */
#define RADIO_SPI SPI1
#define RADIO_SCK_PORT GPIOA
#define RADIO_SCK_PIN GPIO_PIN_5
#define RADIO_MISO_PORT GPIOA
#define RADIO_MISO_PIN GPIO_PIN_6
#define RADIO_MOSI_PORT GPIOA
#define RADIO_MOSI_PIN GPIO_PIN_7
#define RADIO_NSS_PORT GPIOA
#define RADIO_NSS_PIN GPIO_PIN_4
#define RADIO_RESET_PORT GPIOB
#define RADIO_RESET_PIN GPIO_PIN_0
#define RADIO_DIO0_PORT GPIOB
#define RADIO_DIO0_PIN GPIO_PIN_1 /* rising edge on transmit done and receive done */
#define RADIO_DIO0_IRQN EXTI1_IRQn
#define RADIO_DIO0_IRQHANDLER EXTI1_IRQHandler
#define RADIO_BUSY_PORT GPIOA
#define RADIO_BUSY_PIN GPIO_PIN_8 /* handshake from SPI radios that have one */

/* UART radios (AT-command modules) on USART3; they share the reset line above */
#define RADIO_UART USART3
#define RADIO_UART_IRQN USART3_IRQn
#define RADIO_UART_IRQHANDLER USART3_IRQHandler
#define RADIO_UART_CLK_ENABLE() __HAL_RCC_USART3_CLK_ENABLE()
#define RADIO_UART_BAUD 115200u
#define RADIO_UART_TX_PORT GPIOB
#define RADIO_UART_TX_PIN GPIO_PIN_10 /* to the module's RXD */
#define RADIO_UART_RX_PORT GPIOB
#define RADIO_UART_RX_PIN GPIO_PIN_11 /* from the module's TXD */

/* Second SPI for storage or a second radio */
#define AUX_SPI SPI2
#define AUX_SCK_PORT GPIOB
#define AUX_SCK_PIN GPIO_PIN_13
#define AUX_MISO_PORT GPIOB
#define AUX_MISO_PIN GPIO_PIN_14
#define AUX_MOSI_PORT GPIOB
#define AUX_MOSI_PIN GPIO_PIN_15
#define AUX_NSS_PORT GPIOB
#define AUX_NSS_PIN GPIO_PIN_12

/* Sensor bus on I2C1, 100 kHz. The F1 cannot pull up an open-drain peripheral pin, so the bus needs its own
   pull-up resistors (most sensor breakouts carry them). */
#define SENSOR_I2C I2C1
#define SENSOR_SCL_PORT GPIOB
#define SENSOR_SCL_PIN GPIO_PIN_6
#define SENSOR_SDA_PORT GPIOB
#define SENSOR_SDA_PIN GPIO_PIN_7

/* Line protocol on USART1. The board has no debug probe with a serial port, so a USB serial adapter goes here:
   its RX to PA9, its TX to PA10, grounds joined. */
#define HOST_UART USART1
#define HOST_UART_IRQN USART1_IRQn
#define HOST_UART_IRQHANDLER USART1_IRQHandler
#define HOST_UART_CLK_ENABLE() __HAL_RCC_USART1_CLK_ENABLE()
#define HOST_UART_BAUD 115200u
#define HOST_TX_PORT GPIOA
#define HOST_TX_PIN GPIO_PIN_9
#define HOST_RX_PORT GPIOA
#define HOST_RX_PIN GPIO_PIN_10

/* Battery sense on ADC1 through a resistor divider; the channel number is in mcu.h */
#define BATTERY_PORT GPIOA
#define BATTERY_PIN GPIO_PIN_0

#endif
