/* Interrupt handlers for the Nucleo-L476RG firmware. */
#include "board.h"

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(RADIO_DIO0_PIN);
}

void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&board_host_uart);
}

void UART4_IRQHandler(void)
{
    HAL_UART_IRQHandler(&board_radio_uart);
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}
