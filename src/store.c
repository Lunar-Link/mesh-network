/* Message store: checksummed records, lifetime countdown, eviction order, message-id counter. */
#include <string.h>

#include "dedup.h"
#include "store.h"

_Static_assert(LMESH_MSGID_BOOT_MARGIN > LMESH_MSGID_SAVE_EVERY, "restore margin must cover the ids issued between saves");

#define MSGID_SLOT_LEN 4u
#define SWEEP_PERIOD_MS 1000u
#define REBASE_HELD_MS 0x40000000u

uint16_t lmesh_crc16(const uint8_t *p, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ ((uint16_t)p[i] << 8));
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint16_t crc_u8(uint16_t crc, uint8_t v)
{
    return lmesh_crc16(&v, 1, crc);
}

static uint16_t crc_u16(uint16_t crc, uint16_t v)
{
    uint8_t b[2];
    lmesh_put16(b, v);
    return lmesh_crc16(b, sizeof b, crc);
}

static uint16_t crc_u32(uint16_t crc, uint32_t v)
{
    uint8_t b[4];
    lmesh_put32(b, v);
    return lmesh_crc16(b, sizeof b, crc);
}

uint16_t lmesh_record_crc(const lmesh_record *r)
{
    uint16_t crc = LMESH_CRC16_INIT;
    crc = crc_u8(crc, r->origin);
    crc = crc_u8(crc, r->dest);
    crc = crc_u16(crc, r->id);
    crc = crc_u8(crc, r->type_prio);
    crc = crc_u8(crc, r->hops);
    crc = crc_u32(crc, r->lifetime_s);
    crc = crc_u8(crc, r->content_len);
    crc = lmesh_crc16(r->content, sizeof r->content, crc);
    crc = crc_u8(crc, r->role);
    crc = crc_u8(crc, r->state);
    crc = crc_u8(crc, r->attempts);
    crc = crc_u8(crc, r->result_code);
    crc = crc_u8(crc, r->in_flight_to);
    crc = crc_u32(crc, r->acquired_ms);
    crc = crc_u32(crc, r->next_attempt_ms);
    crc = crc_u32(crc, r->e2e_backoff_ms);
    crc = crc_u32(crc, r->submitted_ms);
    return crc;
}

void lmesh_record_seal(lmesh_record *r)
{
    r->crc = lmesh_record_crc(r);
}

bool lmesh_record_valid(const lmesh_record *r)
{
    return r->crc == lmesh_record_crc(r);
}

static bool is_active(uint8_t role)
{
    return role == LMESH_ROLE_ORIGIN_COPY || role == LMESH_ROLE_ORIGIN_SHADOW || role == LMESH_ROLE_RELAY;
}

static bool is_origin(uint8_t role)
{
    return role == LMESH_ROLE_ORIGIN_COPY || role == LMESH_ROLE_ORIGIN_SHADOW;
}

static void save_msg_id(lmesh_node *node)
{
    if (!node->port.storage_write) {
        return;
    }
    uint8_t b[MSGID_SLOT_LEN];
    lmesh_put16(b, node->store.next_msg_id);
    lmesh_put16(b + 2, lmesh_crc16(b, 2, LMESH_CRC16_INIT));
    (void)node->port.storage_write(node->port.ctx, LMESH_SLOT_MSGID, b, sizeof b);
    node->store.ids_since_save = 0;
}

void lmesh_store_init(lmesh_node *node)
{
    uint8_t b[MSGID_SLOT_LEN];
    uint16_t next = 1;
    if (node->port.storage_read && node->port.storage_read(node->port.ctx, LMESH_SLOT_MSGID, b, sizeof b) == 0 &&
        lmesh_get16(b + 2) == lmesh_crc16(b, 2, LMESH_CRC16_INIT)) {
        next = (uint16_t)(lmesh_get16(b) + LMESH_MSGID_BOOT_MARGIN);
        if (next == 0) {
            next = 1;
        }
    }
    node->store.next_msg_id = next;
    node->store.ids_since_save = 0;
    node->store.last_sweep_ms = node->now_ms;
    save_msg_id(node);
    lmesh_dedup_load(node);
}

uint16_t lmesh_store_next_msg_id(lmesh_node *node)
{
    uint16_t id = node->store.next_msg_id;
    if (id == 0) {
        id = 1;
    }
    node->store.next_msg_id = (uint16_t)(id + 1u);
    if (node->store.next_msg_id == 0) {
        node->store.next_msg_id = 1;
    }
    node->store.ids_since_save++;
    if (node->store.ids_since_save >= LMESH_MSGID_SAVE_EVERY) {
        save_msg_id(node);
    }
    return id;
}

uint32_t lmesh_record_remaining_s(const lmesh_node *node, const lmesh_record *r)
{
    uint32_t held_s = (node->now_ms - r->acquired_ms) / 1000u;
    return r->lifetime_s > held_s ? r->lifetime_s - held_s : 0u;
}

static void emit_state(lmesh_node *node, const lmesh_record *r, lmesh_fail_reason reason)
{
    lmesh_event ev = {.type = LMESH_EV_MSG_STATE, .t_ms = node->now_ms};
    ev.u.msg_state.ref.origin = r->origin;
    ev.u.msg_state.ref.id = r->id;
    ev.u.msg_state.dest = r->dest;
    ev.u.msg_state.msg_type = lmesh_record_type(r);
    ev.u.msg_state.prio = lmesh_record_prio(r);
    ev.u.msg_state.state = (lmesh_msg_state)r->state;
    ev.u.msg_state.reason = (uint8_t)reason;
    ev.u.msg_state.result_code = r->result_code;
    lmesh_emit(node, &ev);
}

static void emit_store(lmesh_node *node)
{
    lmesh_event ev = {.type = LMESH_EV_STORE, .t_ms = node->now_ms};
    ev.u.store.depth = lmesh_store_depth(node);
    ev.u.store.evicted = node->store.evicted;
    ev.u.store.expired = node->store.expired;
    lmesh_emit(node, &ev);
}

static void discard_corrupt(lmesh_node *node, lmesh_record *r)
{
    memset(r, 0, sizeof *r);
    node->stats.record_crc_errors++;
}

static lmesh_record *pick_nearest(lmesh_node *node, uint8_t role, lmesh_prio lo, lmesh_prio hi)
{
    lmesh_record *best = NULL;
    uint32_t best_left = 0;
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        lmesh_record *r = &node->store.records[i];
        lmesh_prio p = lmesh_record_prio(r);
        if (r->role != role || p < lo || p > hi) {
            continue;
        }
        uint32_t left = lmesh_record_remaining_s(node, r);
        if (!best || left < best_left) {
            best = r;
            best_left = left;
        }
    }
    return best;
}

static lmesh_record *pick_victim(lmesh_node *node, lmesh_prio arriving)
{
    lmesh_record *v = pick_nearest(node, LMESH_ROLE_RELAY, LMESH_PRIO_ROUTINE, LMESH_PRIO_ROUTINE);
    if (!v) {
        v = pick_nearest(node, LMESH_ROLE_RELAY, LMESH_PRIO_NORMAL, LMESH_PRIO_NORMAL);
    }
    if (!v) {
        lmesh_prio lo = arriving == LMESH_PRIO_URGENT ? LMESH_PRIO_URGENT : LMESH_PRIO_NORMAL;
        v = pick_nearest(node, LMESH_ROLE_ORIGIN_SHADOW, lo, LMESH_PRIO_ROUTINE);
    }
    for (int p = LMESH_PRIO_ROUTINE; !v && p > (int)arriving; p--) {
        v = pick_nearest(node, LMESH_ROLE_ORIGIN_COPY, (lmesh_prio)p, (lmesh_prio)p);
    }
    return v;
}

lmesh_record *lmesh_store_alloc(lmesh_node *node, lmesh_prio arriving, uint8_t role)
{
    lmesh_record *slot = NULL;
    lmesh_record *oldest_history = NULL;

    for (size_t i = 0; i < LMESH_STORE_SIZE && !slot; i++) {
        if (node->store.records[i].role == LMESH_ROLE_FREE) {
            slot = &node->store.records[i];
        }
    }
    for (size_t i = 0; i < LMESH_STORE_SIZE && !slot; i++) {
        lmesh_record *r = &node->store.records[i];
        if (!lmesh_record_valid(r)) {
            discard_corrupt(node, r);
            slot = r;
        }
    }
    for (size_t i = 0; i < LMESH_STORE_SIZE && !slot; i++) {
        lmesh_record *r = &node->store.records[i];
        if (r->role == LMESH_ROLE_HISTORY &&
            (!oldest_history || (int32_t)(r->acquired_ms - oldest_history->acquired_ms) < 0)) {
            oldest_history = r;
        }
    }
    if (!slot) {
        slot = oldest_history;
    }
    if (!slot) {
        slot = pick_victim(node, arriving);
        if (!slot) {
            return NULL;
        }
        if (is_origin(slot->role)) {
            slot->state = LMESH_STATE_FAILED;
            emit_state(node, slot, LMESH_REASON_EVICTED);
        }
        node->stats.evicted++;
        node->store.evicted++;
        slot->role = LMESH_ROLE_FREE;
        emit_store(node);
    }

    memset(slot, 0, sizeof *slot);
    slot->role = role;
    slot->acquired_ms = node->now_ms;
    lmesh_record_seal(slot);
    return slot;
}

lmesh_record *lmesh_store_find(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t role_mask)
{
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        lmesh_record *r = &node->store.records[i];
        if (r->role != LMESH_ROLE_FREE && r->role <= LMESH_ROLE_HISTORY && (role_mask & LMESH_ROLE_MASK(r->role)) &&
            r->origin == origin && r->id == id && lmesh_record_valid(r)) {
            return r;
        }
    }
    return NULL;
}

void lmesh_store_release(lmesh_node *node, lmesh_record *r)
{
    (void)node;
    memset(r, 0, sizeof *r);
}

void lmesh_store_retire(lmesh_node *node, lmesh_record *r, lmesh_msg_state state, lmesh_fail_reason reason)
{
    if (!is_origin(r->role)) {
        if (r->role == LMESH_ROLE_RELAY) {
            lmesh_store_release(node, r);
        }
        return;
    }
    r->state = (uint8_t)state;
    emit_state(node, r, reason);
    r->role = LMESH_ROLE_HISTORY;
    r->acquired_ms = node->now_ms;
    lmesh_record_seal(r);
}

uint8_t lmesh_store_depth(const lmesh_node *node)
{
    uint8_t depth = 0;
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        if (is_active(node->store.records[i].role)) {
            depth++;
        }
    }
    return depth;
}

void lmesh_store_tick(lmesh_node *node)
{
    uint32_t due = node->store.last_sweep_ms + SWEEP_PERIOD_MS;
    if (!lmesh_time_reached(node->now_ms, due)) {
        lmesh_wake_at(node, due);
        return;
    }
    node->store.last_sweep_ms = node->now_ms;

    bool changed = false;
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        lmesh_record *r = &node->store.records[i];
        if (r->role == LMESH_ROLE_FREE) {
            continue;
        }
        if (!lmesh_record_valid(r)) {
            discard_corrupt(node, r);
            continue;
        }
        if (r->role == LMESH_ROLE_HISTORY) {
            if (node->now_ms - r->acquired_ms >= LMESH_HISTORY_HOLD_MS) {
                lmesh_store_release(node, r);
            }
            continue;
        }
        if (lmesh_record_remaining_s(node, r) == 0) {
            node->stats.expired++;
            node->store.expired++;
            changed = true;
            if (is_origin(r->role)) {
                lmesh_store_retire(node, r, LMESH_STATE_EXPIRED, LMESH_REASON_LIFETIME);
            } else {
                lmesh_store_release(node, r);
            }
            continue;
        }
        uint32_t held_ms = node->now_ms - r->acquired_ms;
        if (held_ms >= REBASE_HELD_MS) {
            uint32_t held_s = held_ms / 1000u;
            r->lifetime_s -= held_s;
            r->acquired_ms += held_s * 1000u;
            lmesh_record_seal(r);
        }
    }
    if (changed) {
        emit_store(node);
    }
    lmesh_wake_at(node, node->store.last_sweep_ms + SWEEP_PERIOD_MS);
}
