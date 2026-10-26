/* Message layer: submit, forward, deliver, receipts, states, weighted round-robin scheduler. SPEC.md Section 9. */
#ifndef LMESH_MESSAGE_H
#define LMESH_MESSAGE_H

#include "frame.h"

#define LMESH_MSG_TYPE_MASK 0x0Fu
#define LMESH_MSG_PRIO_SHIFT 4
#define LMESH_MSG_PRIO_MASK 0x30u
#define LMESH_MSG_FLAG_RESEND 0x40u

lmesh_err lmesh_message_submit(lmesh_node *node, lmesh_node_id dest, lmesh_msg_type type, lmesh_prio prio,
                               uint32_t lifetime_s, const uint8_t *content, size_t len, lmesh_msg_ref *out);
lmesh_err lmesh_message_cancel(lmesh_node *node, lmesh_msg_ref ref);
void lmesh_message_rx_data(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len);
void lmesh_message_link_result(lmesh_node *node, uint16_t record_index, uint8_t ack_status);
void lmesh_message_route_event(lmesh_node *node, lmesh_node_id dest);
void lmesh_message_tick(lmesh_node *node);

#endif
