/* Status broadcasts and the node table. SPEC.md Section 10. */
#ifndef LMESH_STATUS_H
#define LMESH_STATUS_H

#include "frame.h"

enum lmesh_health_flag {
    LMESH_HEALTH_LOW_BATTERY = 0x01,
    LMESH_HEALTH_STORE_HIGH = 0x02,
    LMESH_HEALTH_RADIO_FAULT = 0x04,
    LMESH_HEALTH_RESET = 0x08,
    LMESH_HEALTH_ISOLATED = 0x10,
    LMESH_HEALTH_APP_MASK = 0xE0
};

#define LMESH_STATUS_FIXED_LEN 11u
#define LMESH_STATUS_BODY_MAX (LMESH_STATUS_FIXED_LEN + LMESH_STATION_ID_MAX + 2u * LMESH_MAX_NEIGHBORS)

void lmesh_status_init(lmesh_node *node);
void lmesh_status_tick(lmesh_node *node);
size_t lmesh_status_encode(lmesh_node *node, uint8_t *out, size_t out_max);
lmesh_err lmesh_status_send(lmesh_node *node);
void lmesh_status_rx(lmesh_node *node, const uint8_t *payload, size_t len);
lmesh_node_entry *lmesh_status_entry(lmesh_node *node, lmesh_node_id id);

#endif
