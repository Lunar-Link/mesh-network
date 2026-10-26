/* HELLO transmission and reception: periodic and triggered sends, route-list pagination, decoding. */
#include "hello.h"
#include "link.h"
#include "neighbor.h"
#include "route.h"

#define HELLO_FIXED_LEN 6u
#define HELLO_NEIGHBOR_LEN 2u
#define HELLO_ROUTE_LEN 4u
#define HELLO_JITTER_MS ((uint32_t)LMESH_HELLO_PERIOD_MS * LMESH_HELLO_JITTER_PCT / 100u)
#define NO_SLOT 0xFFu

typedef struct hello_entry {
    lmesh_node_id dest;
    uint8_t metric8;
    uint8_t retract_slot;
    uint16_t seqno;
} hello_entry;

static size_t route_list(lmesh_node *node, hello_entry *list)
{
    size_t m = 0;
    list[m++] = (hello_entry){node->ident.id, 0, NO_SLOT, node->route.own_seqno};
    for (size_t s = 0; s < LMESH_MAX_NODES; s++) {
        const lmesh_route *r = &node->route.table[s];
        if (r->used && r->metric8 != LMESH_METRIC_INF) {
            list[m++] = (hello_entry){r->dest, r->metric8, NO_SLOT, r->seqno};
        }
    }
    for (size_t s = 0; s < LMESH_MAX_NODES; s++) {
        const lmesh_route *r = &node->route.table[s];
        if (r->used && r->metric8 == LMESH_METRIC_INF && r->retract_left > 0) {
            list[m++] = (hello_entry){r->dest, LMESH_METRIC_INF, (uint8_t)s, r->seqno};
        }
    }
    return m;
}

static size_t encode(lmesh_node *node, uint8_t *out, size_t out_max, uint8_t flags)
{
    size_t max = out_max < LMESH_MAX_PAYLOAD ? out_max : LMESH_MAX_PAYLOAD;
    if (!out || max < HELLO_FIXED_LEN + HELLO_ROUTE_LEN) {
        return 0;
    }

    size_t pos = HELLO_FIXED_LEN - 1u;
    size_t neighbor_room = (max - HELLO_FIXED_LEN - HELLO_ROUTE_LEN) / HELLO_NEIGHBOR_LEN;
    uint8_t n_count = 0;
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS && n_count < neighbor_room; i++) {
        const lmesh_neighbor *n = &node->nbr.table[i];
        if (n->used && n->up) {
            out[pos++] = n->id;
            out[pos++] = n->rx_quality;
            n_count++;
        }
    }

    hello_entry list[LMESH_MAX_NODES + 1];
    size_t total = route_list(node, list);
    size_t per_page = (max - pos - 1u) / HELLO_ROUTE_LEN;
    size_t first = (size_t)node->hello.page * per_page;
    if (first >= total) {
        first = 0;
    }
    size_t last = first + per_page < total ? first + per_page : total;

    out[pos++] = (uint8_t)(last - first);
    for (size_t k = first; k < last; k++) {
        out[pos] = list[k].dest;
        lmesh_put16(out + pos + 1, list[k].seqno);
        out[pos + 3] = list[k].metric8;
        pos += HELLO_ROUTE_LEN;
        if (list[k].retract_slot != NO_SLOT) {
            node->route.table[list[k].retract_slot].retract_left--;
        }
    }
    if (last < total) {
        flags = (uint8_t)(flags | LMESH_HELLO_MORE);
        node->hello.page = (uint8_t)(first / per_page + 1u);
    } else {
        node->hello.page = 0;
    }

    out[0] = node->nbr.hello_seq++;
    lmesh_put16(out + 1, node->route.own_seqno);
    out[3] = flags;
    out[4] = n_count;
    return pos;
}

size_t lmesh_hello_encode(lmesh_node *node, uint8_t *out, size_t out_max)
{
    return encode(node, out, out_max, 0);
}

static void send_hello(lmesh_node *node, uint8_t flags)
{
    uint8_t buf[LMESH_MAX_PAYLOAD];
    size_t len = encode(node, buf, sizeof buf, flags);
    if (len > 0) {
        (void)lmesh_link_send(node, LMESH_FT_HELLO, LMESH_ID_ALL, buf, len, LMESH_PRIO_NORMAL, node->now_ms, false,
                              LMESH_OWNER_HELLO, 0);
    }
}

void lmesh_hello_init(lmesh_node *node)
{
    lmesh_hello_state *h = &node->hello;
    h->trigger_pending = false;
    h->page = 0;
    h->next_ms = node->now_ms + lmesh_rand_range(node, 0, LMESH_HELLO_PERIOD_MS);
    h->last_triggered_ms = node->now_ms - LMESH_TRIGGER_MIN_GAP_MS;
    lmesh_wake_at(node, h->next_ms);
}

void lmesh_hello_tick(lmesh_node *node)
{
    lmesh_hello_state *h = &node->hello;
    bool periodic = lmesh_time_reached(node->now_ms, h->next_ms);
    bool triggered = h->trigger_pending && (periodic || lmesh_time_reached(node->now_ms, h->trigger_at_ms));

    if (periodic || triggered) {
        send_hello(node, triggered ? (uint8_t)LMESH_HELLO_TRIGGERED : 0u);
    }
    if (triggered) {
        h->trigger_pending = false;
        h->last_triggered_ms = node->now_ms;
    }
    if (periodic) {
        h->next_ms = node->now_ms + lmesh_rand_range(node, LMESH_HELLO_PERIOD_MS - HELLO_JITTER_MS,
                                                     LMESH_HELLO_PERIOD_MS + HELLO_JITTER_MS);
    }
    if (!h->trigger_pending && lmesh_time_reached(node->now_ms, h->last_triggered_ms + LMESH_TRIGGER_MIN_GAP_MS)) {
        h->last_triggered_ms = node->now_ms - LMESH_TRIGGER_MIN_GAP_MS;
    }
    lmesh_wake_at(node, h->next_ms);
    if (h->trigger_pending) {
        lmesh_wake_at(node, h->trigger_at_ms);
    }
}

void lmesh_hello_trigger(lmesh_node *node)
{
    lmesh_hello_state *h = &node->hello;
    if (!h->trigger_pending) {
        uint32_t at = node->now_ms + lmesh_rand_range(node, 0, LMESH_TRIGGER_DELAY_MS);
        uint32_t earliest = h->last_triggered_ms + LMESH_TRIGGER_MIN_GAP_MS;
        if ((int32_t)(at - earliest) < 0) {
            at = earliest;
        }
        h->trigger_pending = true;
        h->trigger_at_ms = at;
    }
    lmesh_wake_at(node, h->trigger_at_ms);
}

static bool well_formed(const uint8_t *p, size_t len)
{
    if (!p || len < HELLO_FIXED_LEN) {
        return false;
    }
    size_t n = p[4];
    size_t route_count_at = HELLO_FIXED_LEN - 1u + n * HELLO_NEIGHBOR_LEN;
    if (len < route_count_at + 1u) {
        return false;
    }
    size_t m = p[route_count_at];
    return len == route_count_at + 1u + m * HELLO_ROUTE_LEN;
}

void lmesh_hello_rx(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len, int16_t rssi,
                    int8_t snr)
{
    if (!hdr || !well_formed(payload, len) || hdr->tx == LMESH_ID_NONE || hdr->tx == LMESH_ID_ALL) {
        node->stats.rx_malformed++;
        return;
    }
    lmesh_node_id tx = hdr->tx;
    lmesh_node_id self = node->ident.id;
    lmesh_neighbor *n = lmesh_neighbor_touch(node, tx, rssi, snr);
    if (!n) {
        return;
    }

    size_t n_count = payload[4];
    const uint8_t *p = payload + HELLO_FIXED_LEN - 1u;
    bool listed_us = false;
    uint8_t their_quality = 0;
    for (size_t i = 0; i < n_count; i++, p += HELLO_NEIGHBOR_LEN) {
        if (p[0] == self) {
            listed_us = true;
            their_quality = p[1];
        }
    }
    lmesh_neighbor_hello_seen(node, n, payload[0], listed_us, their_quality);
    lmesh_route_update(node, n, tx, lmesh_get16(payload + 1), 0);

    size_t m = *p++;
    for (size_t k = 0; k < m; k++, p += HELLO_ROUTE_LEN) {
        if (p[0] == self || p[0] == tx) {
            continue;
        }
        lmesh_route_update(node, n, p[0], lmesh_get16(p + 1), p[3]);
    }
}
