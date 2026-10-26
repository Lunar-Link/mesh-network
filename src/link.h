/* Transmit queue, hop-acknowledged unicast, retries, link failure counting. SPEC.md Section 6. */
#ifndef LMESH_LINK_H
#define LMESH_LINK_H

#include "frame.h"

#define LMESH_HOP_ACK_FRAME_LEN (LMESH_HEADER_LEN + 2 + LMESH_TAG_LEN)

enum lmesh_tx_owner {
    LMESH_OWNER_NONE = 0,
    LMESH_OWNER_HELLO,
    LMESH_OWNER_MESSAGE,
    LMESH_OWNER_FLOOD
};

lmesh_err lmesh_link_send(lmesh_node *node, uint8_t type, lmesh_node_id to, const uint8_t *payload, size_t len,
                          uint8_t band, uint32_t due_ms, bool want_ack, uint8_t owner, uint16_t owner_ref);
bool lmesh_link_cancel(lmesh_node *node, uint8_t owner, uint16_t owner_ref);
void lmesh_link_send_ack(lmesh_node *node, lmesh_node_id to, uint8_t seq, uint8_t status);
bool lmesh_link_is_repeat(lmesh_node *node, const lmesh_frame_hdr *hdr);
void lmesh_link_rx_ack(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len);
void lmesh_link_tx_done(lmesh_node *node, bool ok);
void lmesh_link_neighbor_reset(lmesh_node *node, lmesh_node_id id);
void lmesh_link_tick(lmesh_node *node);
uint32_t lmesh_link_slot_ms(const lmesh_node *node);

#endif
