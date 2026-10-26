/* Board application for the STM32L476RG. The platform port (SPEC.md Section 12) will drive the mesh from here. */
#include "board.h"

int main(void)
{
    HAL_Init();
    board_init();
    for (;;) {
    }
}
