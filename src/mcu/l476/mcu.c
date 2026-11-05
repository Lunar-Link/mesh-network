/* STM32L476RG: 80 MHz clock from the MSI, I2C1, ADC1, and a random seed from the hardware RNG. */
#include "board.h"

static RNG_HandleTypeDef rng;

/* 80 MHz from the PLL on the 4 MHz MSI, which needs no crystal: 4 * 40 / 2. Voltage range 1 is needed above 26 MHz. */
void mcu_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) {
        board_fail();
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
        board_fail();
    }
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) {
        board_fail();
    }
}

/* 100 kHz standard mode; the L4 I2C takes a timing word, this one for an 80 MHz I2C clock. */
void mcu_i2c_init(void)
{
    I2C_HandleTypeDef *h = &board_sensor_i2c;
    h->Instance = SENSOR_I2C;
    h->Init.Timing = 0x10909CECu;
    h->Init.OwnAddress1 = 0;
    h->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    h->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    h->Init.OwnAddress2 = 0;
    h->Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    h->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    h->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(h) != HAL_OK || HAL_I2CEx_ConfigAnalogFilter(h, I2C_ANALOGFILTER_ENABLE) != HAL_OK ||
        HAL_I2CEx_ConfigDigitalFilter(h, 0) != HAL_OK) {
        board_fail();
    }
}

/* ADC1 on the 80 MHz system clock divided by 4, calibrated once; each read configures its channel. */
void mcu_adc_init(void)
{
    ADC_HandleTypeDef *h = &board_adc;

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
    if (HAL_ADC_Init(h) != HAL_OK || HAL_ADCEx_Calibration_Start(h, ADC_SINGLE_ENDED) != HAL_OK) {
        board_fail();
    }
}

bool mcu_adc_read(uint32_t channel, uint32_t *raw)
{
    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel = channel;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
    ch.SingleDiff = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset = 0;
    if (HAL_ADC_ConfigChannel(&board_adc, &ch) != HAL_OK || HAL_ADC_Start(&board_adc) != HAL_OK ||
        HAL_ADC_PollForConversion(&board_adc, 10) != HAL_OK) {
        HAL_ADC_Stop(&board_adc);
        return false;
    }
    *raw = HAL_ADC_GetValue(&board_adc);
    HAL_ADC_Stop(&board_adc);
    return true;
}

/* One word from the hardware RNG, which needs a 48 MHz clock: PLLSAI1 from the 4 MHz MSI, x24 to 96 MHz, Q divides by
   2. The RNG is stopped again afterwards, since nothing else uses it. If it reports a seed error, the device id and
   the tick stand in; only jitter and the first flood sequence number depend on it. */
uint32_t board_entropy(void)
{
    RCC_PeriphCLKInitTypeDef p = {0};
    uint32_t v = 0;
    p.PeriphClockSelection = RCC_PERIPHCLK_RNG;
    p.RngClockSelection = RCC_RNGCLKSOURCE_PLLSAI1;
    p.PLLSAI1.PLLSAI1Source = RCC_PLLSOURCE_MSI;
    p.PLLSAI1.PLLSAI1M = 1;
    p.PLLSAI1.PLLSAI1N = 24;
    p.PLLSAI1.PLLSAI1P = RCC_PLLP_DIV7;
    p.PLLSAI1.PLLSAI1Q = RCC_PLLQ_DIV2;
    p.PLLSAI1.PLLSAI1R = RCC_PLLR_DIV2;
    p.PLLSAI1.PLLSAI1ClockOut = RCC_PLLSAI1_48M2CLK;
    rng.Instance = RNG;
    if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK || HAL_RNG_Init(&rng) != HAL_OK ||
        HAL_RNG_GenerateRandomNumber(&rng, &v) != HAL_OK) {
        v = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^ HAL_GetTick() ^ SysTick->VAL;
    }
    HAL_RNG_DeInit(&rng);
    return v;
}

void HAL_RNG_MspInit(RNG_HandleTypeDef *h)
{
    if (h->Instance == RNG) {
        __HAL_RCC_RNG_CLK_ENABLE();
    }
}

void HAL_RNG_MspDeInit(RNG_HandleTypeDef *h)
{
    if (h->Instance == RNG) {
        __HAL_RCC_RNG_CLK_DISABLE();
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
            board_fail();
        }
        __HAL_RCC_ADC_CLK_ENABLE();
        g.Pin = BATTERY_PIN;
        g.Mode = GPIO_MODE_ANALOG_ADC_CONTROL;
        g.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(BATTERY_PORT, &g);
    }
}
