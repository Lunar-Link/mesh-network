/* Board setup shared by the Nucleo-F446RE and Nucleo-L476RG: pins, SPI, UARTs, battery sense, and the radio interrupt
   line. The clock, I2C, ADC and random seed are per chip, in src/mcu/<chip>/mcu.c. */
#include "board.h"

SPI_HandleTypeDef board_radio_spi;
SPI_HandleTypeDef board_aux_spi;
I2C_HandleTypeDef board_sensor_i2c;
UART_HandleTypeDef board_host_uart;
UART_HandleTypeDef board_radio_uart;
ADC_HandleTypeDef board_adc;

void board_fail(void)
{
    __disable_irq();
    for (;;) {
    }
}

static void output(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level)
{
    GPIO_InitTypeDef g = {0};
    HAL_GPIO_WritePin(port, pin, level);
    g.Pin = pin;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &g);
}

static void alternate(GPIO_TypeDef *port, uint16_t pin, uint32_t af, uint32_t mode, uint32_t pull)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = pin;
    g.Mode = mode;
    g.Pull = pull;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = af;
    HAL_GPIO_Init(port, &g);
}

static void gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    output(RADIO_NSS_PORT, RADIO_NSS_PIN, GPIO_PIN_SET);
    output(RADIO_RESET_PORT, RADIO_RESET_PIN, GPIO_PIN_SET);
    output(AUX_NSS_PORT, AUX_NSS_PIN, GPIO_PIN_SET);

    g.Pin = RADIO_DIO0_PIN;
    g.Mode = GPIO_MODE_IT_RISING;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(RADIO_DIO0_PORT, &g);
    HAL_NVIC_SetPriority(RADIO_DIO0_IRQN, 5, 0);
    HAL_NVIC_EnableIRQ(RADIO_DIO0_IRQN);

    g.Pin = RADIO_BUSY_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(RADIO_BUSY_PORT, &g);

    g.Pin = BUTTON_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(BUTTON_PORT, &g);
}

static void spi_init(SPI_HandleTypeDef *h, SPI_TypeDef *instance, uint32_t prescaler)
{
    h->Instance = instance;
    h->Init.Mode = SPI_MODE_MASTER;
    h->Init.Direction = SPI_DIRECTION_2LINES;
    h->Init.DataSize = SPI_DATASIZE_8BIT;
    h->Init.CLKPolarity = SPI_POLARITY_LOW;
    h->Init.CLKPhase = SPI_PHASE_1EDGE;
    h->Init.NSS = SPI_NSS_SOFT;
    h->Init.BaudRatePrescaler = prescaler;
    h->Init.FirstBit = SPI_FIRSTBIT_MSB;
    h->Init.TIMode = SPI_TIMODE_DISABLE;
    h->Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    h->Init.CRCPolynomial = 7;
    if (HAL_SPI_Init(h) != HAL_OK) {
        board_fail();
    }
}

static void uart_init(UART_HandleTypeDef *h, USART_TypeDef *instance, uint32_t baud)
{
    h->Instance = instance;
    h->Init.BaudRate = baud;
    h->Init.WordLength = UART_WORDLENGTH_8B;
    h->Init.StopBits = UART_STOPBITS_1;
    h->Init.Parity = UART_PARITY_NONE;
    h->Init.Mode = UART_MODE_TX_RX;
    h->Init.HwFlowCtl = UART_HWCONTROL_NONE;
    h->Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(h) != HAL_OK) {
        board_fail();
    }
}

void board_init(void)
{
    mcu_clock_init();
    gpio_init();
    spi_init(&board_radio_spi, RADIO_SPI, RADIO_SPI_PRESCALER);
    spi_init(&board_aux_spi, AUX_SPI, AUX_SPI_PRESCALER);
    mcu_i2c_init();
    uart_init(&board_host_uart, HOST_UART, HOST_UART_BAUD);
    uart_init(&board_radio_uart, RADIO_UART, RADIO_UART_BAUD);
    mcu_adc_init();
}

void board_radio_reset(bool asserted)
{
    HAL_GPIO_WritePin(RADIO_RESET_PORT, RADIO_RESET_PIN, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

uint16_t board_battery_mv(void)
{
    uint32_t raw;
    if (!mcu_adc_read(BATTERY_ADC_CHANNEL, &raw)) {
        return 0;
    }
    return (uint16_t)(raw * ADC_VREF_MV * BATTERY_DIVIDER_NUM / (4095u * BATTERY_DIVIDER_DEN));
}

/* HAL hooks: pin and clock setup per peripheral, called from the HAL init functions above. */

void HAL_SPI_MspInit(SPI_HandleTypeDef *h)
{
    if (h->Instance == RADIO_SPI) {
        __HAL_RCC_SPI1_CLK_ENABLE();
        alternate(RADIO_SCK_PORT, RADIO_SCK_PIN, GPIO_AF5_SPI1, GPIO_MODE_AF_PP, GPIO_NOPULL);
        alternate(RADIO_MISO_PORT, RADIO_MISO_PIN, GPIO_AF5_SPI1, GPIO_MODE_AF_PP, GPIO_NOPULL);
        alternate(RADIO_MOSI_PORT, RADIO_MOSI_PIN, GPIO_AF5_SPI1, GPIO_MODE_AF_PP, GPIO_NOPULL);
    } else if (h->Instance == AUX_SPI) {
        __HAL_RCC_SPI2_CLK_ENABLE();
        alternate(AUX_SCK_PORT, AUX_SCK_PIN, GPIO_AF5_SPI2, GPIO_MODE_AF_PP, GPIO_NOPULL);
        alternate(AUX_MISO_PORT, AUX_MISO_PIN, GPIO_AF5_SPI2, GPIO_MODE_AF_PP, GPIO_NOPULL);
        alternate(AUX_MOSI_PORT, AUX_MOSI_PIN, GPIO_AF5_SPI2, GPIO_MODE_AF_PP, GPIO_NOPULL);
    }
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *h)
{
    if (h->Instance == SENSOR_I2C) {
        __HAL_RCC_I2C1_CLK_ENABLE();
        alternate(SENSOR_SCL_PORT, SENSOR_SCL_PIN, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
        alternate(SENSOR_SDA_PORT, SENSOR_SDA_PIN, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
    }
}

void HAL_UART_MspInit(UART_HandleTypeDef *h)
{
    if (h->Instance == HOST_UART) {
        __HAL_RCC_USART2_CLK_ENABLE();
        alternate(HOST_TX_PORT, HOST_TX_PIN, GPIO_AF7_USART2, GPIO_MODE_AF_PP, GPIO_NOPULL);
        alternate(HOST_RX_PORT, HOST_RX_PIN, GPIO_AF7_USART2, GPIO_MODE_AF_PP, GPIO_NOPULL);
        HAL_NVIC_SetPriority(HOST_UART_IRQN, 6, 0);
        HAL_NVIC_EnableIRQ(HOST_UART_IRQN);
    } else if (h->Instance == RADIO_UART) {
        __HAL_RCC_UART4_CLK_ENABLE();
        alternate(RADIO_UART_TX_PORT, RADIO_UART_TX_PIN, GPIO_AF8_UART4, GPIO_MODE_AF_PP, GPIO_NOPULL);
        alternate(RADIO_UART_RX_PORT, RADIO_UART_RX_PIN, GPIO_AF8_UART4, GPIO_MODE_AF_PP, GPIO_PULLUP);
        HAL_NVIC_SetPriority(RADIO_UART_IRQN, 5, 0);
        HAL_NVIC_EnableIRQ(RADIO_UART_IRQN);
    }
}
