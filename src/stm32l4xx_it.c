/* Interrupt handlers for the Nucleo-L476RG firmware. */
#include "board.h"
#include "storage.h"

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

/* A flash double ECC error raises NMI; the storage scan treats the read as failed and carries on. */
void NMI_Handler(void)
{
    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCD)) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD);
        storage_ecc_error();
        return;
    }
    for (;;) {
    }
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}
