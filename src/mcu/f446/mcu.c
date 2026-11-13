/* STM32F446RE: 84 MHz clock from the HSI, I2C1, ADC1, and a random seed from ADC noise (the F446 has no RNG). */
#include "board.h"

/* 84 MHz from the PLL on the 16 MHz HSI, which needs no crystal or solder bridge: 16 / 8 * 168 / 4. APB1 runs at
   42 MHz (limit 45), APB2 at 84 MHz (limit 90). Voltage scale 3 covers up to 120 MHz and draws the least. */
void mcu_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = 8;
    osc.PLL.PLLN = 168;
    osc.PLL.PLLP = RCC_PLLP_DIV4;
    osc.PLL.PLLQ = 7;
    osc.PLL.PLLR = 2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        board_fail();
    }
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        board_fail();
    }
}

/* 100 kHz standard mode; the F4 I2C takes the bus speed directly. */
void mcu_i2c_init(void)
{
    I2C_HandleTypeDef *h = &board_sensor_i2c;
    h->Instance = SENSOR_I2C;
    h->Init.ClockSpeed = 100000u;
    h->Init.DutyCycle = I2C_DUTYCYCLE_2;
    h->Init.OwnAddress1 = 0;
    h->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    h->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    h->Init.OwnAddress2 = 0;
    h->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    h->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(h) != HAL_OK) {
        board_fail();
    }
}

/* ADC1 at 21 MHz (APB2 / 4); each read configures its channel, so the battery and the temperature sensor share it. */
void mcu_adc_init(void)
{
    ADC_HandleTypeDef *h = &board_adc;

    h->Instance = ADC1;
    h->Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    h->Init.Resolution = ADC_RESOLUTION_12B;
    h->Init.DataAlign = ADC_DATAALIGN_RIGHT;
    h->Init.ScanConvMode = DISABLE;
    h->Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    h->Init.ContinuousConvMode = DISABLE;
    h->Init.NbrOfConversion = 1;
    h->Init.DiscontinuousConvMode = DISABLE;
    h->Init.ExternalTrigConv = ADC_SOFTWARE_START;
    h->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    h->Init.DMAContinuousRequests = DISABLE;
    if (HAL_ADC_Init(h) != HAL_OK) {
        board_fail();
    }
}

bool mcu_adc_read(uint32_t channel, uint32_t *raw)
{
    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel = channel;
    ch.Rank = 1;
    ch.SamplingTime = ADC_SAMPLETIME_480CYCLES; /* 23 us, over the temperature sensor's 10 us minimum */
    if (HAL_ADC_ConfigChannel(&board_adc, &ch) != HAL_OK || HAL_ADC_Start(&board_adc) != HAL_OK ||
        HAL_ADC_PollForConversion(&board_adc, 10) != HAL_OK) {
        HAL_ADC_Stop(&board_adc);
        return false;
    }
    *raw = HAL_ADC_GetValue(&board_adc);
    HAL_ADC_Stop(&board_adc);
    return true;
}

/* The low bits of the temperature sensor and the battery pin wander from read to read; 64 reads folded together with
   the device id and the SysTick counter, FNV-1a style, differ on every start. Enough for jitter, not for keys. */
uint32_t board_entropy(void)
{
    uint32_t h = 2166136261u ^ HAL_GetUIDw0() ^ (HAL_GetUIDw1() * 31u) ^ (HAL_GetUIDw2() * 961u);
    for (int i = 0; i < 64; i++) {
        uint32_t raw = 0;
        (void)mcu_adc_read((i & 1) ? BATTERY_ADC_CHANNEL : ADC_CHANNEL_TEMPSENSOR, &raw);
        h = (h ^ raw ^ SysTick->VAL) * 16777619u;
    }
    return h;
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *h)
{
    if (h->Instance == ADC1) {
        GPIO_InitTypeDef g = {0};
        __HAL_RCC_ADC1_CLK_ENABLE();
        g.Pin = BATTERY_PIN;
        g.Mode = GPIO_MODE_ANALOG;
        g.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(BATTERY_PORT, &g);
    }
}
