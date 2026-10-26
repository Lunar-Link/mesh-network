/* Board application for the STM32L476RG. Empty until the platform port (SPEC.md Section 12) is written. */
#include "stm32l4xx_hal.h"

void SysTick_Handler(void)
{
    HAL_IncTick();
}

int main(void)
{
    HAL_Init();
    for (;;) {
    }
}
