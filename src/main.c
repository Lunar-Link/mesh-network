/* Board application: brings up the board, the console and the radio, then runs the mesh from the main loop. */
#include "board.h"
#include "console.h"
#include "port.h"
#include "radio.h"
#include "storage.h"

static lmesh_node node;

int main(void)
{
    lmesh_port port;
    lmesh_identity ident;
    lmesh_callbacks cb = {.on_event = console_event};

    HAL_Init();
    board_init();
    console_init();
    storage_init();
    radio_init();
    port_init(&port);
    port_identity(&ident);
    if (lmesh_init(&node, &ident, &port, &cb) != LMESH_OK) {
        console_line("{\"ev\":\"error\",\"what\":\"lmesh_init\"}");
        for (;;) {
            __WFI();
        }
    }

    /* SysTick wakes the core every millisecond, so __WFI never sleeps past a due timer. */
    uint32_t wake = HAL_GetTick();
    for (;;) {
        bool work = console_poll(&node);
        work = radio_poll(&node) || work;
        if (work || (int32_t)(HAL_GetTick() - wake) >= 0) {
            lmesh_tick(&node);
            wake = HAL_GetTick() + lmesh_next_wake_ms(&node);
        }
        __WFI();
    }
}
