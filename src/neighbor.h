/* Neighbor table: liveness, HELLO loss tracking, bidirectional ETX. SPEC.md Section 7. */
#ifndef LMESH_NEIGHBOR_H
#define LMESH_NEIGHBOR_H

#include "internal.h"

lmesh_neighbor *lmesh_neighbor_find(lmesh_node *node, lmesh_node_id id);
lmesh_neighbor *lmesh_neighbor_touch(lmesh_node *node, lmesh_node_id id, int16_t rssi, int8_t snr);
void lmesh_neighbor_hello_seen(lmesh_node *node, lmesh_neighbor *n, uint8_t hello_seq, bool listed_us,
                               uint8_t their_rx_quality);
void lmesh_neighbor_link_result(lmesh_node *node, lmesh_node_id id, bool ok);
void lmesh_neighbor_tick(lmesh_node *node);
uint8_t lmesh_neighbor_etx8(const lmesh_neighbor *n);
uint8_t lmesh_etx8_compute(uint8_t rx_quality, uint8_t tx_quality, bool tx_reported);

#endif
