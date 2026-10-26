/* Duplicate suppression: per-origin sliding windows, command result replay, relay recent-forward cache, persistence. */
#include <string.h>

#include "dedup.h"
#include "store.h"

_Static_assert(LMESH_FWD_CACHE >= 1 && LMESH_FWD_CACHE <= 256, "forward cache index is uint8_t");

#define SAVE_ENTRY_LEN 7u
#define SAVE_LEN (SAVE_ENTRY_LEN * LMESH_MAX_NODES + 2u)

lmesh_dedup_window *lmesh_dedup_window_for(lmesh_node *node, lmesh_node_id origin)
{
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        lmesh_dedup_window *w = &node->dedup.windows[i];
        if (w->used && w->origin == origin) {
            return w;
        }
    }
    return NULL;
}

static lmesh_dedup_window *window_claim(lmesh_node *node)
{
    lmesh_dedup_window *lru = NULL;
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        lmesh_dedup_window *w = &node->dedup.windows[i];
        if (!w->used) {
            return w;
        }
        if (!lru || (int32_t)(w->last_used_ms - lru->last_used_ms) < 0) {
            lru = w;
        }
    }
    return lru;
}

static void flush(lmesh_node *node)
{
    if (node->dedup.dirty && lmesh_time_reached(node->now_ms, node->dedup.last_save_ms + LMESH_DEDUP_SAVE_GAP_MS)) {
        lmesh_dedup_save(node);
    }
}

static void mark_dirty(lmesh_node *node)
{
    if (node->port.storage_write) {
        node->dedup.dirty = true;
    }
}

bool lmesh_dedup_dest_accept(lmesh_node *node, lmesh_node_id origin, uint16_t id)
{
    lmesh_dedup_window *w = lmesh_dedup_window_for(node, origin);
    bool accept = true;

    if (!w) {
        w = window_claim(node);
        memset(w, 0, sizeof *w);
        w->used = true;
        w->origin = origin;
        w->highest = id;
        w->bitmap = 1u;
        mark_dirty(node);
    } else if (lmesh_seqno_newer(id, w->highest)) {
        uint16_t shift = (uint16_t)(id - w->highest);
        w->bitmap = shift < LMESH_DEDUP_WINDOW_BITS ? (w->bitmap << shift) | 1u : 1u;
        w->highest = id;
        mark_dirty(node);
    } else {
        uint16_t back = (uint16_t)(w->highest - id);
        if (back >= LMESH_DEDUP_WINDOW_BITS || (w->bitmap & (1u << back))) {
            accept = false;
        } else {
            w->bitmap |= 1u << back;
            mark_dirty(node);
        }
    }
    w->last_used_ms = node->now_ms;
    flush(node);
    return accept;
}

void lmesh_dedup_remember_result(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t code)
{
    lmesh_dedup_window *w = lmesh_dedup_window_for(node, origin);
    if (w) {
        w->last_cmd_id = id;
        w->last_cmd_result = code;
        w->cmd_valid = true;
    }
}

bool lmesh_dedup_lookup_result(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t *code)
{
    lmesh_dedup_window *w = lmesh_dedup_window_for(node, origin);
    if (!w || !w->cmd_valid || w->last_cmd_id != id) {
        return false;
    }
    if (code) {
        *code = w->last_cmd_result;
    }
    return true;
}

static bool fwd_fresh(const lmesh_node *node, const lmesh_fwd_entry *e)
{
    return e->used && node->now_ms - e->t_ms < LMESH_FWD_CACHE_HOLD_MS;
}

bool lmesh_dedup_relay_accept(lmesh_node *node, lmesh_node_id origin, uint16_t id)
{
    for (size_t i = 0; i < LMESH_FWD_CACHE; i++) {
        const lmesh_fwd_entry *e = &node->dedup.fwd[i];
        if (fwd_fresh(node, e) && e->origin == origin && e->id == id) {
            return false;
        }
    }
    lmesh_fwd_entry *slot = &node->dedup.fwd[node->dedup.fwd_next];
    slot->used = true;
    slot->origin = origin;
    slot->id = id;
    slot->t_ms = node->now_ms;
    node->dedup.fwd_next = (uint8_t)((node->dedup.fwd_next + 1u) % LMESH_FWD_CACHE);
    return true;
}

void lmesh_dedup_save(lmesh_node *node)
{
    if (!node->port.storage_write) {
        return;
    }
    uint8_t buf[SAVE_LEN];
    memset(buf, 0, sizeof buf);
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        const lmesh_dedup_window *w = &node->dedup.windows[i];
        uint8_t *p = buf + i * SAVE_ENTRY_LEN;
        if (w->used) {
            p[0] = w->origin;
            lmesh_put16(p + 1, w->highest);
            lmesh_put32(p + 3, w->bitmap);
        }
    }
    lmesh_put16(buf + SAVE_LEN - 2u, lmesh_crc16(buf, SAVE_LEN - 2u, LMESH_CRC16_INIT));
    (void)node->port.storage_write(node->port.ctx, LMESH_SLOT_DEDUP, buf, sizeof buf);
    node->dedup.dirty = false;
    node->dedup.last_save_ms = node->now_ms;
}

void lmesh_dedup_load(lmesh_node *node)
{
    uint8_t buf[SAVE_LEN];
    node->dedup.dirty = false;
    node->dedup.last_save_ms = node->now_ms - LMESH_DEDUP_SAVE_GAP_MS;
    if (!node->port.storage_read || node->port.storage_read(node->port.ctx, LMESH_SLOT_DEDUP, buf, sizeof buf) != 0 ||
        lmesh_get16(buf + SAVE_LEN - 2u) != lmesh_crc16(buf, SAVE_LEN - 2u, LMESH_CRC16_INIT)) {
        return;
    }
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        const uint8_t *p = buf + i * SAVE_ENTRY_LEN;
        lmesh_dedup_window *w = &node->dedup.windows[i];
        memset(w, 0, sizeof *w);
        if (p[0] == LMESH_ID_NONE || p[0] == LMESH_ID_ALL) {
            continue;
        }
        w->used = true;
        w->origin = p[0];
        w->highest = lmesh_get16(p + 1);
        w->bitmap = lmesh_get32(p + 3);
        w->last_used_ms = node->now_ms;
    }
}

void lmesh_dedup_tick(lmesh_node *node)
{
    for (size_t i = 0; i < LMESH_FWD_CACHE; i++) {
        lmesh_fwd_entry *e = &node->dedup.fwd[i];
        if (e->used && !fwd_fresh(node, e)) {
            e->used = false;
        }
    }
    flush(node);
    if (node->dedup.dirty) {
        lmesh_wake_at(node, node->dedup.last_save_ms + LMESH_DEDUP_SAVE_GAP_MS);
    }
}
