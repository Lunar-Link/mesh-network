/* Platform port: SysTick clock, hardware random numbers, the selected radio driver, battery sense, flash storage. */
#include "port.h"

#include <string.h>

#include "board.h"
#include "node_config.h"
#include "radio.h"
#include "storage.h"

_Static_assert(NODE_ID >= 1 && NODE_ID <= 254, "NODE_ID must be 1 to 254");
_Static_assert(sizeof STATION_ID - 1 <= LMESH_STATION_ID_MAX, "STATION_ID is too long");

static uint32_t fallback;

static uint32_t port_now_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

/* The RNG can report a seed error; a xorshift seeded from the device id covers that case, since only jitter needs it. */
static uint32_t port_random(void *ctx)
{
    (void)ctx;
    uint32_t v;
    if (HAL_RNG_GenerateRandomNumber(&board_rng, &v) == HAL_OK) {
        return v;
    }
    if (fallback == 0) {
        fallback = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^ HAL_GetTick() ^ 0x9e3779b9u;
    }
    fallback ^= fallback << 13;
    fallback ^= fallback >> 17;
    fallback ^= fallback << 5;
    return fallback;
}

static int port_radio_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return radio_send(frame, len);
}

static uint32_t port_airtime_ms(void *ctx, size_t len)
{
    (void)ctx;
    return radio_airtime_ms(len);
}

static uint16_t port_battery_mv(void *ctx)
{
    (void)ctx;
    return board_battery_mv();
}

static int port_storage_read(void *ctx, uint8_t slot, void *buf, size_t len)
{
    (void)ctx;
    return storage_read(slot, buf, len);
}

static int port_storage_write(void *ctx, uint8_t slot, const void *buf, size_t len)
{
    (void)ctx;
    return storage_write(slot, buf, len);
}

void port_init(lmesh_port *port)
{
    memset(port, 0, sizeof *port);
    port->now_ms = port_now_ms;
    port->random = port_random;
    port->radio_send = port_radio_send;
    port->airtime_ms = port_airtime_ms;
    port->battery_mv = port_battery_mv;
    port->storage_read = port_storage_read;
    port->storage_write = port_storage_write;
}

void port_identity(lmesh_identity *ident)
{
    static const uint8_t key[16] = NETWORK_KEY;
    memset(ident, 0, sizeof *ident);
    ident->id = NODE_ID;
    ident->network_id = NETWORK_ID;
    memcpy(ident->key, key, sizeof key);
    ident->station_id_len = (uint8_t)(sizeof STATION_ID - 1);
    memcpy(ident->station_id, STATION_ID, sizeof STATION_ID - 1);
}
