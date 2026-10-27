/* Board setup for the Nucleo-L476RG: 80 MHz clock, pins, SPI, I2C, UARTs, ADC, RNG, and the radio interrupt line. */
#include "board.h"

SPI_HandleTypeDef board_radio_spi;
SPI_HandleTypeDef board_aux_spi;
I2C_HandleTypeDef board_sensor_i2c;
UART_HandleTypeDef board_host_uart;
UART_HandleTypeDef board_radio_uart;
ADC_HandleTypeDef board_adc;
RNG_HandleTypeDef board_rng;

static void fail(void)
{
    __disable_irq();
    for (;;) {
    }
}

static void clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) {
        fail();
    }
    osc.OscillatorType = RCC_OSCILLATORTYPE_MSI;
    osc.MSIState = RCC_MSI_ON;
    osc.MSICalibrationValue = 0;
    osc.MSIClockRange = RCC_MSIRANGE_6;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_MSI;
    osc.PLL.PLLM = 1;
    osc.PLL.PLLN = 40;
    osc.PLL.PLLP = RCC_PLLP_DIV7;
    osc.PLL.PLLQ = RCC_PLLQ_DIV2;
    osc.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        fail();
    }
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) {
        fail();
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
    h->Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    if (HAL_SPI_Init(h) != HAL_OK) {
        fail();
    }
}

static void i2c_init(void)
{
    I2C_HandleTypeDef *h = &board_sensor_i2c;
    h->Instance = SENSOR_I2C;
    h->Init.Timing = SENSOR_I2C_TIMING;
    h->Init.OwnAddress1 = 0;
    h->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    h->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    h->Init.OwnAddress2 = 0;
    h->Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    h->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    h->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(h) != HAL_OK || HAL_I2CEx_ConfigAnalogFilter(h, I2C_ANALOGFILTER_ENABLE) != HAL_OK ||
        HAL_I2CEx_ConfigDigitalFilter(h, 0) != HAL_OK) {
        fail();
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
    h->Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    h->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(h) != HAL_OK) {
        fail();
    }
}

static void adc_init(void)
{
    ADC_HandleTypeDef *h = &board_adc;
    ADC_ChannelConfTypeDef ch = {0};

    h->Instance = ADC1;
    h->Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;
    h->Init.Resolution = ADC_RESOLUTION_12B;
    h->Init.DataAlign = ADC_DATAALIGN_RIGHT;
    h->Init.ScanConvMode = ADC_SCAN_DISABLE;
    h->Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    h->Init.LowPowerAutoWait = DISABLE;
    h->Init.ContinuousConvMode = DISABLE;
    h->Init.NbrOfConversion = 1;
    h->Init.DiscontinuousConvMode = DISABLE;
    h->Init.ExternalTrigConv = ADC_SOFTWARE_START;
    h->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    h->Init.DMAContinuousRequests = DISABLE;
    h->Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    h->Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(h) != HAL_OK) {
        fail();
    }
    ch.Channel = BATTERY_ADC_CHANNEL;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
    ch.SingleDiff = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset = 0;
    if (HAL_ADC_ConfigChannel(h, &ch) != HAL_OK || HAL_ADCEx_Calibration_Start(h, ADC_SINGLE_ENDED) != HAL_OK) {
        fail();
    }
}

/* The RNG needs a 48 MHz clock: PLLSAI1 from the 4 MHz MSI, x24 to 96 MHz, Q divides by 2. */
static void rng_init(void)
{
    RCC_PeriphCLKInitTypeDef p = {0};
    p.PeriphClockSelection = RCC_PERIPHCLK_RNG;
    p.RngClockSelection = RCC_RNGCLKSOURCE_PLLSAI1;
    p.PLLSAI1.PLLSAI1Source = RCC_PLLSOURCE_MSI;
    p.PLLSAI1.PLLSAI1M = 1;
    p.PLLSAI1.PLLSAI1N = 24;
    p.PLLSAI1.PLLSAI1P = RCC_PLLP_DIV7;
    p.PLLSAI1.PLLSAI1Q = RCC_PLLQ_DIV2;
    p.PLLSAI1.PLLSAI1R = RCC_PLLR_DIV2;
    p.PLLSAI1.PLLSAI1ClockOut = RCC_PLLSAI1_48M2CLK;
    if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) {
        fail();
    }
    board_rng.Instance = RNG;
    if (HAL_RNG_Init(&board_rng) != HAL_OK) {
        fail();
    }
}

void board_init(void)
{
    clock_init();
    gpio_init();
    spi_init(&board_radio_spi, RADIO_SPI, RADIO_SPI_PRESCALER);
    spi_init(&board_aux_spi, AUX_SPI, AUX_SPI_PRESCALER);
    i2c_init();
    uart_init(&board_host_uart, HOST_UART, HOST_UART_BAUD);
    uart_init(&board_radio_uart, RADIO_UART, RADIO_UART_BAUD);
    adc_init();
    rng_init();
}

void board_radio_reset(bool asserted)
{
    HAL_GPIO_WritePin(RADIO_RESET_PORT, RADIO_RESET_PIN, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

uint16_t board_battery_mv(void)
{
    if (HAL_ADC_Start(&board_adc) != HAL_OK || HAL_ADC_PollForConversion(&board_adc, 10) != HAL_OK) {
        HAL_ADC_Stop(&board_adc);
        return 0;
    }
    uint32_t raw = HAL_ADC_GetValue(&board_adc);
    HAL_ADC_Stop(&board_adc);
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
        alternate(SENSOR_SCL_PORT, SENSOR_SCL_PIN, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
        alternate(SENSOR_SDA_PORT, SENSOR_SDA_PIN, GPIO_AF4_I2C1, GPIO_MODE_AF_OD, GPIO_PULLUP);
        __HAL_RCC_I2C1_CLK_ENABLE();
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

void HAL_RNG_MspInit(RNG_HandleTypeDef *h)
{
    if (h->Instance == RNG) {
        __HAL_RCC_RNG_CLK_ENABLE();
    }
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *h)
{
    if (h->Instance == ADC1) {
        RCC_PeriphCLKInitTypeDef p = {0};
        GPIO_InitTypeDef g = {0};
        p.PeriphClockSelection = RCC_PERIPHCLK_ADC;
        p.AdcClockSelection = RCC_ADCCLKSOURCE_SYSCLK;
        if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) {
            fail();
        }
        __HAL_RCC_ADC_CLK_ENABLE();
        g.Pin = BATTERY_PIN;
        g.Mode = GPIO_MODE_ANALOG_ADC_CONTROL;
        g.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(BATTERY_PORT, &g);
    }
}
