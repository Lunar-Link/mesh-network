/* Pin map for the Nucleo-64 boards (Nucleo-F446RE, Nucleo-L476RG). Both have the same headers, and every pin below
   has the same alternate function on both chips, so the wiring is the same. Included through the chip's mcu.h. */
#ifndef PINS_NUCLEO64_H
#define PINS_NUCLEO64_H

/* Radio on SPI1 with the pins most LoRa shields use on the Arduino header */
#define RADIO_SPI SPI1
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
#define RADIO_DIO0_IRQHANDLER EXTI15_10_IRQHandler
#define RADIO_BUSY_PORT GPIOA
#define RADIO_BUSY_PIN GPIO_PIN_9 /* D8, handshake from SPI radios that have one */

/* UART radios (AT-command modules) on UART4 at the Arduino A0/A1 pins; they share the reset line above */
#define RADIO_UART UART4
#define RADIO_UART_IRQN UART4_IRQn
#define RADIO_UART_IRQHANDLER UART4_IRQHandler
#define RADIO_UART_CLK_ENABLE() __HAL_RCC_UART4_CLK_ENABLE()
#define RADIO_UART_AF GPIO_AF8_UART4
#define RADIO_UART_BAUD 115200u
#define RADIO_UART_TX_PORT GPIOA
#define RADIO_UART_TX_PIN GPIO_PIN_0 /* A0, to the module's RXD */
#define RADIO_UART_RX_PORT GPIOA
#define RADIO_UART_RX_PIN GPIO_PIN_1 /* A1, from the module's TXD */

/* Second SPI on the Morpho headers for storage or a second radio */
#define AUX_SPI SPI2
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
#define SENSOR_SCL_PORT GPIOB
#define SENSOR_SCL_PIN GPIO_PIN_8 /* D15 */
#define SENSOR_SDA_PORT GPIOB
#define SENSOR_SDA_PIN GPIO_PIN_9 /* D14 */

/* Line protocol on USART2, which the ST-LINK presents to the laptop as a virtual COM port */
#define HOST_UART USART2
#define HOST_UART_IRQN USART2_IRQn
#define HOST_UART_IRQHANDLER USART2_IRQHandler
#define HOST_UART_CLK_ENABLE() __HAL_RCC_USART2_CLK_ENABLE()
#define HOST_UART_AF GPIO_AF7_USART2
#define HOST_UART_BAUD 115200u
#define HOST_TX_PORT GPIOA
#define HOST_TX_PIN GPIO_PIN_2
#define HOST_RX_PORT GPIOA
#define HOST_RX_PIN GPIO_PIN_3

/* Battery sense on ADC1 through a resistor divider; the channel number is per chip, in mcu.h */
#define BATTERY_PORT GPIOC
#define BATTERY_PIN GPIO_PIN_0 /* A5 */

/* User button B1 */
#define BUTTON_PORT GPIOC
#define BUTTON_PIN GPIO_PIN_13

#endif
