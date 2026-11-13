/* No radio: frames go nowhere and finish after the simulator's airtime model. For console testing on one board. */
#include "board.h"
#include "radio.h"

static bool on_air;
static uint32_t done_ms;

void radio_init(void)
{
}

uint32_t radio_airtime_ms(size_t len)
{
    return 15u + (uint32_t)((len * 1500u + 999u) / 1000u);
}

int radio_send(const uint8_t *frame, size_t len)
{
    (void)frame;
    if (on_air) {
        return -1;
    }
    on_air = true;
    done_ms = HAL_GetTick() + radio_airtime_ms(len);
    return 0;
}

bool radio_poll(lmesh_node *node)
{
    if (on_air && (int32_t)(HAL_GetTick() - done_ms) >= 0) {
        on_air = false;
        lmesh_radio_tx_done(node, true);
        return true;
    }
    return false;
}
