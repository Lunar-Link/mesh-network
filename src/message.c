/* Message layer. Stub: implemented in phase P4. */
#include "message.h"

lmesh_err lmesh_message_submit(lmesh_node *node, lmesh_node_id dest, lmesh_msg_type type, lmesh_prio prio,
                               uint32_t lifetime_s, const uint8_t *content, size_t len, lmesh_msg_ref *out)
{
    (void)node;
    (void)dest;
    (void)type;
    (void)prio;
    (void)lifetime_s;
    (void)content;
    (void)len;
    (void)out;
    return LMESH_ERR_UNIMPLEMENTED;
}

lmesh_err lmesh_message_cancel(lmesh_node *node, lmesh_msg_ref ref)
{
    (void)node;
    (void)ref;
    return LMESH_ERR_UNIMPLEMENTED;
}

void lmesh_message_rx_data(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len)
{
    (void)node;
    (void)hdr;
    (void)payload;
    (void)len;
}

void lmesh_message_link_result(lmesh_node *node, uint16_t record_index, uint8_t ack_status)
{
    (void)node;
    (void)record_index;
    (void)ack_status;
}

void lmesh_message_route_event(lmesh_node *node, lmesh_node_id dest)
{
    (void)node;
    (void)dest;
}

void lmesh_message_tick(lmesh_node *node)
{
    (void)node;
}
