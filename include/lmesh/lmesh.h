/* Public API of the lunar mesh library. See SPEC.md Section 12. */
#ifndef LMESH_H
#define LMESH_H

#include "lmesh_config.h"
#include "lmesh_port.h"
#include "lmesh_state.h"
#include "lmesh_types.h"

lmesh_err lmesh_init(lmesh_node *node, const lmesh_identity *ident, const lmesh_port *port, const lmesh_callbacks *cb);
void lmesh_tick(lmesh_node *node);
uint32_t lmesh_next_wake_ms(const lmesh_node *node);

void lmesh_radio_rx(lmesh_node *node, const uint8_t *frame, size_t len, int16_t rssi, int8_t snr);
void lmesh_radio_tx_done(lmesh_node *node, bool ok);

lmesh_err lmesh_send(lmesh_node *node, lmesh_node_id dest, lmesh_msg_type type, lmesh_prio prio, uint32_t lifetime_s,
                     const uint8_t *content, size_t len, lmesh_msg_ref *out);
lmesh_err lmesh_cancel(lmesh_node *node, lmesh_msg_ref ref);
lmesh_err lmesh_status_now(lmesh_node *node);

size_t lmesh_neighbors(const lmesh_node *node, lmesh_neighbor_info *out, size_t max);
size_t lmesh_routes(const lmesh_node *node, lmesh_route_info *out, size_t max);
size_t lmesh_messages(const lmesh_node *node, lmesh_msg_info *out, size_t max);
size_t lmesh_nodes(const lmesh_node *node, lmesh_node_info *out, size_t max);
void lmesh_get_stats(const lmesh_node *node, lmesh_stats *out);

size_t lmesh_host_command(lmesh_node *node, const char *line, char *out, size_t out_max);
size_t lmesh_host_format_event(const lmesh_event *ev, char *out, size_t out_max);

#endif
