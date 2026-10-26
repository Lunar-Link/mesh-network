/* Node glue: initialization, tick, frame dispatch, events, queries. */
#include <string.h>

#include "dedup.h"
#include "flood.h"
#include "frame.h"
#include "hello.h"
#include "internal.h"
#include "link.h"
#include "message.h"
#include "neighbor.h"
#include "route.h"
#include "status.h"
#include "store.h"

_Static_assert(LMESH_MAX_CONTENT > 0, "frame too small for a message");
_Static_assert(LMESH_STATUS_PERIOD_MS + LMESH_STATUS_JITTER_MS <= 600000, "status period must stay within the identification interval");
_Static_assert(LMESH_MAX_NODES <= 254, "node ids are one byte");
_Static_assert(LMESH_TXQ_SIZE <= 127, "transmit queue index is int8_t");
_Static_assert(LMESH_STORE_SIZE <= 127, "store index is int8_t");

lmesh_err lmesh_init(lmesh_node *node, const lmesh_identity *ident, const lmesh_port *port, const lmesh_callbacks *cb)
{
    if (!node || !ident || !port || !cb) {
        return LMESH_ERR_ARG;
    }
    if (ident->id == LMESH_ID_NONE || ident->id == LMESH_ID_ALL || ident->station_id_len > LMESH_STATION_ID_MAX) {
        return LMESH_ERR_ARG;
    }
    if (!port->now_ms || !port->random || !port->radio_send || !port->airtime_ms) {
        return LMESH_ERR_ARG;
    }

    memset(node, 0, sizeof *node);
    node->ident = *ident;
    node->port = *port;
    node->cb = *cb;
    node->now_ms = port->now_ms(port->ctx);
    node->next_wake_ms = node->now_ms;
    node->link.in_flight = -1;
    node->msg.in_flight = -1;
    node->status.boot_ms = node->now_ms;
    node->status.reset_flag = true;
    node->flood.own_seq = (uint8_t)port->random(port->ctx);
    lmesh_store_init(node);
    lmesh_status_init(node);
    lmesh_route_init(node);

    lmesh_event ev = {.type = LMESH_EV_BOOT, .t_ms = node->now_ms};
    lmesh_emit(node, &ev);
    return LMESH_OK;
}

void lmesh_tick(lmesh_node *node)
{
    node->now_ms = node->port.now_ms(node->port.ctx);
    node->next_wake_ms = node->now_ms + LMESH_TICK_MAX_SLEEP_MS;

    lmesh_neighbor_tick(node);
    lmesh_hello_tick(node);
    lmesh_route_tick(node);
    lmesh_store_tick(node);
    lmesh_dedup_tick(node);
    lmesh_message_tick(node);
    lmesh_status_tick(node);
    lmesh_link_tick(node);
}

uint32_t lmesh_next_wake_ms(const lmesh_node *node)
{
    uint32_t delay = node->next_wake_ms - node->now_ms;
    return (int32_t)delay < 0 ? 0u : delay;
}

void lmesh_radio_rx(lmesh_node *node, const uint8_t *frame, size_t len, int16_t rssi, int8_t snr)
{
    lmesh_frame_hdr hdr;
    const uint8_t *payload = NULL;

    if (lmesh_frame_decode(node, frame, len, &hdr, &payload) != LMESH_OK) {
        return;
    }
    node->stats.rx_frames[hdr.type & 0x0Fu]++;

    switch (hdr.type) {
    case LMESH_FT_HELLO:
        lmesh_hello_rx(node, &hdr, payload, hdr.payload_len, rssi, snr);
        break;
    case LMESH_FT_DATA:
        if (hdr.rx != node->ident.id) {
            break;
        }
        lmesh_neighbor_touch(node, hdr.tx, rssi, snr);
        if (lmesh_link_is_repeat(node, &hdr)) {
            node->stats.dup_hop++;
            lmesh_link_send_ack(node, hdr.tx, hdr.seq, LMESH_ACK_DUPLICATE);
            break;
        }
        lmesh_message_rx_data(node, &hdr, payload, hdr.payload_len);
        break;
    case LMESH_FT_HOP_ACK:
        if (hdr.rx == node->ident.id) {
            lmesh_link_rx_ack(node, &hdr, payload, hdr.payload_len);
        }
        break;
    case LMESH_FT_STATUS:
        if (lmesh_flood_accept(node, &hdr, payload, hdr.payload_len)) {
            lmesh_status_rx(node, payload, hdr.payload_len);
        }
        break;
    case LMESH_FT_ROUTE_REQ:
        if (lmesh_flood_accept(node, &hdr, payload, hdr.payload_len) &&
            hdr.payload_len >= LMESH_FLOOD_HEADER_LEN + 3) {
            lmesh_route_rx_request(node, payload[LMESH_FLOOD_HEADER_LEN],
                                   lmesh_get16(payload + LMESH_FLOOD_HEADER_LEN + 1));
        }
        break;
    default:
        node->stats.rx_malformed++;
        break;
    }
}

void lmesh_radio_tx_done(lmesh_node *node, bool ok)
{
    lmesh_link_tx_done(node, ok);
}

lmesh_err lmesh_send(lmesh_node *node, lmesh_node_id dest, lmesh_msg_type type, lmesh_prio prio, uint32_t lifetime_s,
                     const uint8_t *content, size_t len, lmesh_msg_ref *out)
{
    if (!node || dest == LMESH_ID_NONE || dest == LMESH_ID_ALL || dest == node->ident.id) {
        return LMESH_ERR_ARG;
    }
    if (type == LMESH_MSG_RECEIPT || prio >= LMESH_PRIO_COUNT) {
        return LMESH_ERR_ARG;
    }
    if (len > LMESH_MAX_CONTENT) {
        return LMESH_ERR_TOO_LONG;
    }
    return lmesh_message_submit(node, dest, type, prio, lifetime_s, content, len, out);
}

lmesh_err lmesh_cancel(lmesh_node *node, lmesh_msg_ref ref)
{
    return lmesh_message_cancel(node, ref);
}

lmesh_err lmesh_status_now(lmesh_node *node)
{
    return lmesh_status_send(node);
}

size_t lmesh_neighbors(const lmesh_node *node, lmesh_neighbor_info *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS && n < max; i++) {
        const lmesh_neighbor *nb = &node->nbr.table[i];
        if (!nb->used) {
            continue;
        }
        out[n].id = nb->id;
        out[n].up = nb->up;
        out[n].bidirectional = nb->bidirectional;
        out[n].etx8 = nb->etx8;
        out[n].rx_quality = nb->rx_quality;
        out[n].tx_quality = nb->tx_quality;
        out[n].rssi = nb->rssi;
        out[n].snr = nb->snr;
        out[n].last_heard_ms = nb->last_heard_ms;
        n++;
    }
    return n;
}

size_t lmesh_routes(const lmesh_node *node, lmesh_route_info *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < LMESH_MAX_NODES && n < max; i++) {
        const lmesh_route *r = &node->route.table[i];
        if (!r->used) {
            continue;
        }
        out[n].dest = r->dest;
        out[n].next_hop = r->next_hop;
        out[n].metric8 = r->metric8;
        out[n].seqno = r->seqno;
        out[n].retracted = r->metric8 == LMESH_METRIC_INF;
        n++;
    }
    return n;
}

size_t lmesh_messages(const lmesh_node *node, lmesh_msg_info *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < LMESH_STORE_SIZE && n < max; i++) {
        const lmesh_record *r = &node->store.records[i];
        if (r->role == LMESH_ROLE_FREE) {
            continue;
        }
        out[n].ref.origin = r->origin;
        out[n].ref.id = r->id;
        out[n].dest = r->dest;
        out[n].type = (lmesh_msg_type)(r->type_prio & 0x0Fu);
        out[n].prio = (lmesh_prio)((r->type_prio >> 4) & 0x03u);
        out[n].state = (lmesh_msg_state)r->state;
        out[n].role = r->role;
        out[n].lifetime_s = lmesh_record_remaining_s(node, r);
        out[n].attempts = r->attempts;
        out[n].hops = r->hops;
        n++;
    }
    return n;
}

size_t lmesh_nodes(const lmesh_node *node, lmesh_node_info *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < LMESH_MAX_NODES && n < max; i++) {
        const lmesh_node_entry *e = &node->status.table[i];
        if (!e->used) {
            continue;
        }
        out[n].id = e->id;
        out[n].stale = lmesh_time_reached(node->now_ms, e->last_heard_ms + LMESH_NODE_STALE_MS);
        out[n].battery_mv = e->battery_mv;
        out[n].flags = e->flags;
        out[n].uptime_s = e->uptime_s;
        out[n].store_depth = e->store_depth;
        out[n].lost = e->lost;
        out[n].station_id_len = e->station_id_len;
        memcpy(out[n].station_id, e->station_id, LMESH_STATION_ID_MAX);
        out[n].neighbor_count = e->neighbor_count;
        memcpy(out[n].neighbors, e->neighbors, sizeof e->neighbors);
        memcpy(out[n].neighbor_etx8, e->neighbor_etx8, sizeof e->neighbor_etx8);
        out[n].last_heard_ms = e->last_heard_ms;
        n++;
    }
    return n;
}

void lmesh_get_stats(const lmesh_node *node, lmesh_stats *out)
{
    *out = node->stats;
}

void lmesh_emit(lmesh_node *node, const lmesh_event *ev)
{
    if (node->cb.on_event) {
        node->cb.on_event(node->cb.ctx, ev);
    }
}

void lmesh_wake_at(lmesh_node *node, uint32_t t_ms)
{
    if ((int32_t)(t_ms - node->next_wake_ms) < 0) {
        node->next_wake_ms = t_ms;
    }
}

uint32_t lmesh_rand_range(lmesh_node *node, uint32_t lo, uint32_t hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + node->port.random(node->port.ctx) % (hi - lo + 1u);
}

uint32_t lmesh_airtime(const lmesh_node *node, size_t len)
{
    return node->port.airtime_ms(node->port.ctx, len);
}
