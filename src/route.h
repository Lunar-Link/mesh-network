/* Route table: feasibility condition, selection with hysteresis, retraction, sequence-number requests. SPEC.md Section 8. */
#ifndef LMESH_ROUTE_H
#define LMESH_ROUTE_H

#include "internal.h"

#define LMESH_ROUTE_REQ_BODY_LEN 3u

void lmesh_route_init(lmesh_node *node);
lmesh_route *lmesh_route_find(lmesh_node *node, lmesh_node_id dest);
lmesh_node_id lmesh_route_next_hop(const lmesh_node *node, lmesh_node_id dest);
bool lmesh_route_feasible(const lmesh_route *r, uint16_t seqno, uint8_t metric8);
void lmesh_route_request_body(const lmesh_route *r, uint8_t body[LMESH_ROUTE_REQ_BODY_LEN]);
void lmesh_route_update(lmesh_node *node, lmesh_neighbor *from, lmesh_node_id dest, uint16_t seqno, uint8_t metric8);
void lmesh_route_withdraw(lmesh_node *node, lmesh_neighbor *from, lmesh_node_id dest);
void lmesh_route_neighbor_down(lmesh_node *node, lmesh_node_id id);
void lmesh_route_rx_request(lmesh_node *node, lmesh_node_id dest, uint16_t seqno);
void lmesh_route_tick(lmesh_node *node);

#endif
