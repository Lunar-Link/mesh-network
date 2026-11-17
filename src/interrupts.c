/* Interrupt handlers shared by every board, named through the pin map; the L476 adds its flash ECC NMI in
   src/mcu/l476/flash.c. */
#include "board.h"

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void RADIO_DIO0_IRQHANDLER(void)
{
    HAL_GPIO_EXTI_IRQHandler(RADIO_DIO0_PIN);
}

void HOST_UART_IRQHANDLER(void)
{
    HAL_UART_IRQHandler(&board_host_uart);
}

void RADIO_UART_IRQHANDLER(void)
{
    HAL_UART_IRQHandler(&board_radio_uart);
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}
