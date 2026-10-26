/* Controlled flood: duplicate cache keyed by origin and sequence, hop limit, random rebroadcast delay. SPEC.md Section 6.4. */
#include "flood.h"

#include <string.h>

#include "link.h"

_Static_assert(LMESH_FLOOD_CACHE >= 1 && LMESH_FLOOD_CACHE <= 255, "flood cache index is one byte");
_Static_assert(LMESH_FLOOD_HOPS >= 1 && LMESH_FLOOD_HOPS <= 255, "hop limit is one byte");
_Static_assert(LMESH_MAX_PAYLOAD > LMESH_FLOOD_HEADER_LEN, "frame too small for a flood");

static uint8_t flood_band(uint8_t type)
{
    return type == LMESH_FT_ROUTE_REQ ? (uint8_t)LMESH_PRIO_URGENT : (uint8_t)LMESH_PRIO_ROUTINE;
}

static bool cache_has(const lmesh_flood_state *f, lmesh_node_id origin, uint8_t seq)
{
    for (size_t i = 0; i < LMESH_FLOOD_CACHE; i++) {
        if (f->cache[i].used && f->cache[i].origin == origin && f->cache[i].seq == seq) {
            return true;
        }
    }
    return false;
}

static void cache_put(lmesh_flood_state *f, lmesh_node_id origin, uint8_t seq)
{
    f->cache[f->next].used = true;
    f->cache[f->next].origin = origin;
    f->cache[f->next].seq = seq;
    f->next = (uint8_t)((f->next + 1u) % LMESH_FLOOD_CACHE);
}

lmesh_err lmesh_flood_originate(lmesh_node *node, uint8_t type, const uint8_t *body, size_t len)
{
    if (node == NULL || (type != LMESH_FT_STATUS && type != LMESH_FT_ROUTE_REQ) || (len > 0 && body == NULL)) {
        return LMESH_ERR_ARG;
    }
    if (len > LMESH_MAX_PAYLOAD - LMESH_FLOOD_HEADER_LEN) {
        return LMESH_ERR_TOO_LONG;
    }

    lmesh_flood_state *f = &node->flood;
    uint8_t payload[LMESH_MAX_PAYLOAD];
    payload[0] = node->ident.id;
    payload[1] = f->own_seq;
    payload[2] = LMESH_FLOOD_HOPS;
    if (len > 0) {
        memcpy(&payload[LMESH_FLOOD_HEADER_LEN], body, len);
    }

    lmesh_err err = lmesh_link_send(node, type, LMESH_ID_ALL, payload, len + LMESH_FLOOD_HEADER_LEN, flood_band(type),
                                    node->now_ms, false, LMESH_OWNER_FLOOD, 0);
    if (err != LMESH_OK) {
        return err;
    }
    cache_put(f, node->ident.id, f->own_seq);
    f->own_seq++;
    return LMESH_OK;
}

bool lmesh_flood_accept(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len)
{
    if (node == NULL || hdr == NULL || payload == NULL || len < LMESH_FLOOD_HEADER_LEN || len > LMESH_MAX_PAYLOAD) {
        return false;
    }
    lmesh_node_id origin = payload[0];
    uint8_t seq = payload[1];
    uint8_t hops = payload[2];
    lmesh_flood_state *f = &node->flood;

    if (origin == node->ident.id || origin == LMESH_ID_NONE || origin == LMESH_ID_ALL || cache_has(f, origin, seq)) {
        return false;
    }
    cache_put(f, origin, seq);

    if (hops > 1) {
        uint8_t copy[LMESH_MAX_PAYLOAD];
        memcpy(copy, payload, len);
        copy[2] = (uint8_t)(hops - 1u);
        uint32_t due = node->port.now_ms(node->port.ctx) + lmesh_rand_range(node, 1u, 3u) * lmesh_link_slot_ms(node);
        (void)lmesh_link_send(node, hdr->type, LMESH_ID_ALL, copy, len, flood_band(hdr->type), due, false,
                              LMESH_OWNER_FLOOD, 0);
    }
    return true;
}
