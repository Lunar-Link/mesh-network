/* Duplicate suppression: per-origin sliding windows, command result replay, relay recent-forward cache, persistence. */
#include <string.h>

#include "dedup.h"
#include "store.h"

_Static_assert(LMESH_FWD_CACHE >= 1 && LMESH_FWD_CACHE <= 256, "forward cache index is uint8_t");
_Static_assert(LMESH_DEDUP_WINDOW_BITS >= 32 && LMESH_DEDUP_WINDOW_BITS % 32 == 0 && LMESH_DEDUP_WINDOW_BITS < 0x8000,
               "dedup window is whole 32-bit words and shorter than half the id space");
_Static_assert(LMESH_DEDUP_CMD_HISTORY >= 1 && LMESH_DEDUP_CMD_HISTORY <= 255, "result ring index is uint8_t");

#define WINDOW_WORDS (LMESH_DEDUP_WINDOW_BITS / 32u)
#define SAVE_RING_OFF (3u + 4u * WINDOW_WORDS)
#define SAVE_ENTRY_LEN (SAVE_RING_OFF + 2u + 3u * LMESH_DEDUP_CMD_HISTORY)
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

static bool bit_test(const lmesh_dedup_window *w, uint16_t back)
{
    return (w->bitmap[back / 32u] >> (back % 32u)) & 1u;
}

static void bit_set(lmesh_dedup_window *w, uint16_t back)
{
    w->bitmap[back / 32u] |= (uint32_t)1u << (back % 32u);
}

static void window_shift(lmesh_dedup_window *w, uint16_t shift)
{
    if (shift >= LMESH_DEDUP_WINDOW_BITS) {
        memset(w->bitmap, 0, sizeof w->bitmap);
        return;
    }
    size_t words = shift / 32u;
    unsigned bits = shift % 32u;
    for (size_t i = WINDOW_WORDS; i-- > 0;) {
        uint32_t v = 0;
        if (i >= words) {
            v = w->bitmap[i - words] << bits;
            if (bits && i > words) {
                v |= w->bitmap[i - words - 1u] >> (32u - bits);
            }
        }
        w->bitmap[i] = v;
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
        w->bitmap[0] = 1u;
        mark_dirty(node);
    } else if (lmesh_seqno_newer(id, w->highest)) {
        window_shift(w, (uint16_t)(id - w->highest));
        w->bitmap[0] |= 1u;
        w->highest = id;
        mark_dirty(node);
    } else {
        uint16_t back = (uint16_t)(w->highest - id);
        if (back >= LMESH_DEDUP_WINDOW_BITS) {
            node->stats.dedup_outside++;
        } else if (bit_test(w, back)) {
            accept = false;
        } else {
            bit_set(w, back);
            mark_dirty(node);
        }
    }
    w->last_used_ms = node->now_ms;
    flush(node);
    return accept;
}

static int ring_find(const lmesh_dedup_window *w, uint16_t id)
{
    for (uint8_t i = 0; i < w->cmd_count; i++) {
        if (w->cmd_id[i] == id) {
            return i;
        }
    }
    return -1;
}

void lmesh_dedup_remember_result(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t code)
{
    lmesh_dedup_window *w = lmesh_dedup_window_for(node, origin);
    if (!w) {
        return;
    }
    int at = ring_find(w, id);
    if (at >= 0) {
        w->cmd_code[at] = code;
    } else {
        w->cmd_id[w->cmd_next] = id;
        w->cmd_code[w->cmd_next] = code;
        w->cmd_next = (uint8_t)((w->cmd_next + 1u) % LMESH_DEDUP_CMD_HISTORY);
        if (w->cmd_count < LMESH_DEDUP_CMD_HISTORY) {
            w->cmd_count++;
        }
    }
    mark_dirty(node);
    flush(node);
}

bool lmesh_dedup_lookup_result(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t *code)
{
    lmesh_dedup_window *w = lmesh_dedup_window_for(node, origin);
    int at = w ? ring_find(w, id) : -1;
    if (at < 0) {
        return false;
    }
    if (code) {
        *code = w->cmd_code[at];
    }
    return true;
}

static bool fwd_fresh(const lmesh_node *node, const lmesh_fwd_entry *e)
{
    return e->used && node->now_ms - e->t_ms < LMESH_FWD_CACHE_HOLD_MS;
}

bool lmesh_dedup_relay_seen(const lmesh_node *node, lmesh_node_id origin, uint16_t id)
{
    for (size_t i = 0; i < LMESH_FWD_CACHE; i++) {
        const lmesh_fwd_entry *e = &node->dedup.fwd[i];
        if (fwd_fresh(node, e) && e->origin == origin && e->id == id) {
            return true;
        }
    }
    return false;
}

void lmesh_dedup_relay_remember(lmesh_node *node, lmesh_node_id origin, uint16_t id)
{
    lmesh_fwd_entry *slot = &node->dedup.fwd[node->dedup.fwd_next];
    slot->used = true;
    slot->origin = origin;
    slot->id = id;
    slot->t_ms = node->now_ms;
    node->dedup.fwd_next = (uint8_t)((node->dedup.fwd_next + 1u) % LMESH_FWD_CACHE);
}

bool lmesh_dedup_relay_accept(lmesh_node *node, lmesh_node_id origin, uint16_t id)
{
    if (lmesh_dedup_relay_seen(node, origin, id)) {
        return false;
    }
    lmesh_dedup_relay_remember(node, origin, id);
    return true;
}

/* Image: per window origin, highest, bitmap words, ring count, ring next, ring (id, code); then CRC-16. */
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
        if (!w->used) {
            continue;
        }
        p[0] = w->origin;
        lmesh_put16(p + 1, w->highest);
        for (size_t k = 0; k < WINDOW_WORDS; k++) {
            lmesh_put32(p + 3u + 4u * k, w->bitmap[k]);
        }
        p[SAVE_RING_OFF] = w->cmd_count;
        p[SAVE_RING_OFF + 1u] = w->cmd_next;
        for (size_t k = 0; k < LMESH_DEDUP_CMD_HISTORY; k++) {
            lmesh_put16(p + SAVE_RING_OFF + 2u + 3u * k, w->cmd_id[k]);
            p[SAVE_RING_OFF + 4u + 3u * k] = w->cmd_code[k];
        }
    }
    lmesh_put16(buf + SAVE_LEN - 2u, lmesh_crc16(buf, SAVE_LEN - 2u, LMESH_CRC16_INIT));
    (void)node->port.storage_write(node->port.ctx, LMESH_SLOT_DEDUP, buf, sizeof buf);
    node->dedup.dirty = false;
    node->dedup.last_save_ms = node->now_ms;
}

static bool image_valid(const uint8_t *buf)
{
    if (lmesh_get16(buf + SAVE_LEN - 2u) != lmesh_crc16(buf, SAVE_LEN - 2u, LMESH_CRC16_INIT)) {
        return false;
    }
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        const uint8_t *p = buf + i * SAVE_ENTRY_LEN;
        if (p[SAVE_RING_OFF] > LMESH_DEDUP_CMD_HISTORY || p[SAVE_RING_OFF + 1u] >= LMESH_DEDUP_CMD_HISTORY) {
            return false;
        }
    }
    return true;
}

void lmesh_dedup_load(lmesh_node *node)
{
    uint8_t buf[SAVE_LEN];
    node->dedup.dirty = false;
    node->dedup.last_save_ms = node->now_ms - LMESH_DEDUP_SAVE_GAP_MS;
    if (!node->port.storage_read || node->port.storage_read(node->port.ctx, LMESH_SLOT_DEDUP, buf, sizeof buf) != 0 ||
        !image_valid(buf)) {
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
        for (size_t k = 0; k < WINDOW_WORDS; k++) {
            w->bitmap[k] = lmesh_get32(p + 3u + 4u * k);
        }
        w->cmd_count = p[SAVE_RING_OFF];
        w->cmd_next = p[SAVE_RING_OFF + 1u];
        for (size_t k = 0; k < LMESH_DEDUP_CMD_HISTORY; k++) {
            w->cmd_id[k] = lmesh_get16(p + SAVE_RING_OFF + 2u + 3u * k);
            w->cmd_code[k] = p[SAVE_RING_OFF + 4u + 3u * k];
        }
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
