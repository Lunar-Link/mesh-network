/* Status broadcasts and the node table. SPEC.md Section 10. */
#include <string.h>

#include "flood.h"
#include "status.h"
#include "store.h"

_Static_assert(LMESH_FLOOD_HEADER_LEN + LMESH_STATUS_BODY_MAX <= LMESH_MAX_PAYLOAD,
               "a full status must fit in one frame so the station identifier is always sent");
_Static_assert(LMESH_STATUS_JITTER_MS < LMESH_STATUS_PERIOD_MS, "status jitter must be smaller than the period");

static void update_counters(lmesh_node *node)
{
    lmesh_status_state *s = &node->status;

    uint16_t seen = (uint16_t)(node->store.evicted + node->store.expired);
    uint32_t lost = (uint32_t)s->lost_since + (uint16_t)(seen - s->lost_seen);
    s->lost_seen = seen;
    s->lost_since = (uint8_t)(lost > 255u ? 255u : lost);

    uint32_t secs = (node->now_ms - s->uptime_mark_ms) / 1000u;
    s->uptime_s += secs;
    s->uptime_mark_ms += secs * 1000u;
}

static uint32_t next_period(lmesh_node *node)
{
    return lmesh_rand_range(node, LMESH_STATUS_PERIOD_MS - LMESH_STATUS_JITTER_MS,
                            LMESH_STATUS_PERIOD_MS + LMESH_STATUS_JITTER_MS);
}

void lmesh_status_init(lmesh_node *node)
{
    lmesh_status_state *s = &node->status;

    s->lost_since = 0;
    s->lost_seen = (uint16_t)(node->store.evicted + node->store.expired);
    s->uptime_s = 0;
    s->uptime_mark_ms = s->boot_ms;
    s->next_ms = node->now_ms + lmesh_rand_range(node, 0, LMESH_STATUS_PERIOD_MS);
    lmesh_wake_at(node, s->next_ms);
}

void lmesh_status_tick(lmesh_node *node)
{
    lmesh_status_state *s = &node->status;

    update_counters(node);
    if (lmesh_time_reached(node->now_ms, s->next_ms)) {
        (void)lmesh_status_send(node);
        s->next_ms = node->now_ms + next_period(node);
    }
    lmesh_wake_at(node, s->next_ms);
}

size_t lmesh_status_encode(lmesh_node *node, uint8_t *out, size_t out_max)
{
    const lmesh_identity *ident = &node->ident;
    uint8_t sid_len = ident->station_id_len > LMESH_STATION_ID_MAX ? (uint8_t)LMESH_STATION_ID_MAX
                                                                    : ident->station_id_len;

    size_t up = 0;
    bool usable = false;
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        const lmesh_neighbor *nb = &node->nbr.table[i];
        if (nb->used && nb->up) {
            up++;
            usable = usable || nb->bidirectional;
        }
    }

    size_t len = LMESH_STATUS_FIXED_LEN + sid_len + 2u * up;
    if (len > out_max || len > LMESH_MAX_PAYLOAD - LMESH_FLOOD_HEADER_LEN) {
        return 0;
    }

    update_counters(node);

    uint16_t battery = node->port.battery_mv ? node->port.battery_mv(node->port.ctx) : 0u;
    uint8_t depth = lmesh_store_depth(node);
    uint8_t flags = 0;
    if (battery != 0 && battery < LMESH_LOW_BATTERY_MV) {
        flags |= LMESH_HEALTH_LOW_BATTERY;
    }
    if ((uint32_t)depth * 4u > (uint32_t)LMESH_STORE_SIZE * 3u) {
        flags |= LMESH_HEALTH_STORE_HIGH;
    }
    if (node->status.reset_flag) {
        flags |= LMESH_HEALTH_RESET;
    }
    if (!usable) {
        flags |= LMESH_HEALTH_ISOLATED;
    }
    if (node->port.health_flags) {
        flags |= (uint8_t)(node->port.health_flags(node->port.ctx) & LMESH_HEALTH_APP_MASK);
    }

    lmesh_put16(out, battery);
    out[2] = flags;
    lmesh_put32(out + 3, node->status.uptime_s);
    out[7] = depth;
    out[8] = node->status.lost_since;
    out[9] = sid_len;
    memcpy(out + 10, ident->station_id, sid_len);

    uint8_t *p = out + 10 + sid_len;
    *p++ = (uint8_t)up;
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        const lmesh_neighbor *nb = &node->nbr.table[i];
        if (nb->used && nb->up) {
            *p++ = nb->id;
            *p++ = nb->etx8;
        }
    }
    return len;
}

lmesh_err lmesh_status_send(lmesh_node *node)
{
    uint8_t body[LMESH_STATUS_BODY_MAX];
    size_t len = lmesh_status_encode(node, body, sizeof body);
    if (len == 0) {
        return LMESH_ERR_TOO_LONG;
    }
    lmesh_err err = lmesh_flood_originate(node, LMESH_FT_STATUS, body, len);
    if (err == LMESH_OK) {
        node->status.reset_flag = false;
        node->status.lost_since = 0;
    }
    return err;
}

static lmesh_node_entry *entry_for(lmesh_node *node, lmesh_node_id id)
{
    lmesh_node_entry *e = lmesh_status_entry(node, id);
    if (e) {
        return e;
    }
    lmesh_node_entry *oldest = NULL;
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        e = &node->status.table[i];
        if (!e->used) {
            return e;
        }
        if (!oldest || node->now_ms - e->last_heard_ms > node->now_ms - oldest->last_heard_ms) {
            oldest = e;
        }
    }
    return oldest;
}

void lmesh_status_rx(lmesh_node *node, const uint8_t *payload, size_t len)
{
    if (!payload || len < LMESH_FLOOD_HEADER_LEN + LMESH_STATUS_FIXED_LEN) {
        node->stats.rx_malformed++;
        return;
    }
    lmesh_node_id origin = payload[0];
    const uint8_t *b = payload + LMESH_FLOOD_HEADER_LEN;
    size_t blen = len - LMESH_FLOOD_HEADER_LEN;
    uint8_t sid_len = b[9];
    if (origin == LMESH_ID_NONE || origin == LMESH_ID_ALL || sid_len > LMESH_STATION_ID_MAX ||
        blen < LMESH_STATUS_FIXED_LEN + (size_t)sid_len) {
        node->stats.rx_malformed++;
        return;
    }
    uint8_t count = b[10 + sid_len];
    if (blen != LMESH_STATUS_FIXED_LEN + (size_t)sid_len + 2u * count) {
        node->stats.rx_malformed++;
        return;
    }
    if (origin == node->ident.id) {
        return;
    }

    lmesh_node_entry *e = entry_for(node, origin);
    memset(e, 0, sizeof *e);
    e->used = true;
    e->id = origin;
    e->battery_mv = lmesh_get16(b);
    e->flags = b[2];
    e->uptime_s = lmesh_get32(b + 3);
    e->store_depth = b[7];
    e->lost = b[8];
    e->station_id_len = sid_len;
    memcpy(e->station_id, b + 10, sid_len);
    e->neighbor_count = count > LMESH_MAX_NEIGHBORS ? (uint8_t)LMESH_MAX_NEIGHBORS : count;
    const uint8_t *p = b + 11 + sid_len;
    for (size_t i = 0; i < e->neighbor_count; i++) {
        e->neighbors[i] = p[2 * i];
        e->neighbor_etx8[i] = p[2 * i + 1];
    }
    e->last_heard_ms = node->now_ms;

    lmesh_event ev = {.type = LMESH_EV_STATUS_RECEIVED, .t_ms = node->now_ms};
    ev.u.status.id = origin;
    lmesh_emit(node, &ev);
}

lmesh_node_entry *lmesh_status_entry(lmesh_node *node, lmesh_node_id id)
{
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        lmesh_node_entry *e = &node->status.table[i];
        if (e->used && e->id == id) {
            return e;
        }
    }
    return NULL;
}
