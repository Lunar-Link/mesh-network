/* Message layer: submit, custody forwarding, delivery, receipts, end-to-end re-send, weighted round-robin scheduler. */
#include "message.h"

#include <string.h>

#include "dedup.h"
#include "link.h"
#include "neighbor.h"
#include "route.h"
#include "store.h"

_Static_assert(LMESH_MAX_CONTENT >= 3, "a receipt needs three bytes of content");

#define RECEIPT_FIXED_LEN 3u
#define RECEIPT_LIFETIME_S LMESH_LIFETIME_TEXT_S
#define JITTER_DIV 4u
#define STALE_DUE_MS 0x40000000u
#define ACTIVE_ROLES                                                                                   \
    ((uint8_t)(LMESH_ROLE_MASK(LMESH_ROLE_ORIGIN_COPY) | LMESH_ROLE_MASK(LMESH_ROLE_ORIGIN_SHADOW) | \
               LMESH_ROLE_MASK(LMESH_ROLE_RELAY)))
#define ORIGIN_ROLES ((uint8_t)(LMESH_ROLE_MASK(LMESH_ROLE_ORIGIN_COPY) | LMESH_ROLE_MASK(LMESH_ROLE_ORIGIN_SHADOW)))
#define ALL_ROLES ((uint8_t)(ACTIVE_ROLES | LMESH_ROLE_MASK(LMESH_ROLE_HISTORY)))

static const uint8_t wrr_weight[LMESH_PRIO_COUNT] = {LMESH_WRR_URGENT, LMESH_WRR_NORMAL, LMESH_WRR_ROUTINE};

typedef struct wire_msg {
    lmesh_node_id origin;
    lmesh_node_id dest;
    uint16_t id;
    uint8_t type_prio;
    uint8_t hops;
    bool resend;
    uint32_t lifetime_s;
    const uint8_t *content;
    uint8_t content_len;
} wire_msg;

static uint16_t index_of(const lmesh_node *node, const lmesh_record *r)
{
    return (uint16_t)(r - node->store.records);
}

static bool schedulable(const lmesh_record *r)
{
    return r->role == LMESH_ROLE_ORIGIN_COPY || r->role == LMESH_ROLE_ORIGIN_SHADOW || r->role == LMESH_ROLE_RELAY;
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

static lmesh_node_id next_hop(lmesh_node *node, lmesh_node_id dest)
{
    lmesh_node_id hop = lmesh_route_next_hop(node, dest);
    if (hop != LMESH_ID_NONE) {
        return hop;
    }
    const lmesh_neighbor *n = lmesh_neighbor_find(node, dest);
    return n && n->up && n->bidirectional ? dest : (lmesh_node_id)LMESH_ID_NONE;
}

static uint32_t doubled(uint32_t v, uint32_t cap)
{
    return v >= cap / 2u ? cap : v * 2u;
}

static uint32_t fwd_backoff(lmesh_node *node, uint8_t attempts)
{
    uint32_t base = LMESH_FWD_BACKOFF_MIN_MS;
    for (unsigned i = 1; i < attempts && base < LMESH_FWD_BACKOFF_MAX_MS; i++) {
        base = doubled(base, LMESH_FWD_BACKOFF_MAX_MS);
    }
    if (base > LMESH_FWD_BACKOFF_MAX_MS) {
        base = LMESH_FWD_BACKOFF_MAX_MS;
    }
    uint32_t t = base + lmesh_rand_range(node, 0, base / JITTER_DIV);
    return t > LMESH_FWD_BACKOFF_MAX_MS ? LMESH_FWD_BACKOFF_MAX_MS : t;
}

static bool link_holds(const lmesh_node *node, uint16_t ref)
{
    for (size_t i = 0; i < LMESH_TXQ_SIZE; i++) {
        const lmesh_txq_entry *e = &node->link.queue[i];
        if (e->used && e->owner == LMESH_OWNER_MESSAGE && e->owner_ref == ref) {
            return true;
        }
    }
    return false;
}

static size_t encode(const lmesh_node *node, const lmesh_record *r, uint8_t *buf)
{
    uint8_t len = r->content_len <= LMESH_MAX_CONTENT ? r->content_len : (uint8_t)LMESH_MAX_CONTENT;
    uint8_t flags = r->role == LMESH_ROLE_ORIGIN_SHADOW ? (uint8_t)LMESH_MSG_FLAG_RESEND : 0u;

    buf[0] = r->origin;
    lmesh_put16(buf + 1, r->id);
    buf[3] = r->dest;
    buf[4] = (uint8_t)((r->type_prio & (LMESH_MSG_TYPE_MASK | LMESH_MSG_PRIO_MASK | LMESH_MSG_FLAG_RESEND)) | flags);
    lmesh_put32(buf + 5, lmesh_record_remaining_s(node, r));
    buf[9] = r->hops < UINT8_MAX ? (uint8_t)(r->hops + 1u) : (uint8_t)UINT8_MAX;
    memcpy(buf + LMESH_MSG_HEADER_LEN, r->content, len);
    return LMESH_MSG_HEADER_LEN + (size_t)len;
}

lmesh_err lmesh_message_submit(lmesh_node *node, lmesh_node_id dest, lmesh_msg_type type, lmesh_prio prio,
                               uint32_t lifetime_s, const uint8_t *content, size_t len, lmesh_msg_ref *out)
{
    if (!node || (len > 0 && !content) || type > LMESH_MSG_RECEIPT || prio >= LMESH_PRIO_COUNT) {
        return LMESH_ERR_ARG;
    }
    if (len > LMESH_MAX_CONTENT) {
        return LMESH_ERR_TOO_LONG;
    }
    lmesh_record *r = lmesh_store_alloc(node, prio, LMESH_ROLE_ORIGIN_COPY);
    if (!r) {
        return LMESH_ERR_FULL;
    }
    if (lifetime_s == 0) {
        lifetime_s = type == LMESH_MSG_COMMAND ? LMESH_LIFETIME_COMMAND_S : LMESH_LIFETIME_TEXT_S;
    }
    r->origin = node->ident.id;
    r->id = lmesh_store_next_msg_id(node);
    r->dest = dest;
    r->type_prio = (uint8_t)(((unsigned)type & LMESH_MSG_TYPE_MASK) | ((unsigned)prio << LMESH_MSG_PRIO_SHIFT));
    r->lifetime_s = lifetime_s;
    r->content_len = (uint8_t)len;
    if (len > 0) {
        memcpy(r->content, content, len);
    }
    r->hops = 0;
    r->state = LMESH_STATE_QUEUED;
    r->submitted_ms = node->now_ms;
    r->next_attempt_ms = node->now_ms;
    lmesh_record_seal(r);
    emit_state(node, r, LMESH_REASON_NONE);
    if (out) {
        out->origin = r->origin;
        out->id = r->id;
    }
    lmesh_wake_at(node, node->now_ms);
    return LMESH_OK;
}

lmesh_err lmesh_message_cancel(lmesh_node *node, lmesh_msg_ref ref)
{
    if (!node) {
        return LMESH_ERR_ARG;
    }
    if (ref.origin != node->ident.id) {
        return LMESH_ERR_NOT_FOUND;
    }
    lmesh_record *r = lmesh_store_find(node, ref.origin, ref.id, ALL_ROLES);
    if (!r) {
        return LMESH_ERR_NOT_FOUND;
    }
    if (r->role != LMESH_ROLE_ORIGIN_COPY || r->state != LMESH_STATE_QUEUED) {
        return LMESH_ERR_STATE;
    }
    uint16_t idx = index_of(node, r);
    if (node->msg.in_flight == (int)idx) {
        if (!lmesh_link_cancel(node, LMESH_OWNER_MESSAGE, idx)) {
            return LMESH_ERR_STATE;
        }
        node->msg.in_flight = -1;
        lmesh_wake_at(node, node->now_ms);
    }
    r->in_flight_to = LMESH_ID_NONE;
    lmesh_store_retire(node, r, LMESH_STATE_FAILED, LMESH_REASON_CANCELED);
    return LMESH_OK;
}

static bool parse(const lmesh_node *node, const uint8_t *p, size_t len, wire_msg *m)
{
    if (!p || len < LMESH_MSG_HEADER_LEN || len - LMESH_MSG_HEADER_LEN > LMESH_MAX_CONTENT) {
        return false;
    }
    m->origin = p[0];
    m->id = lmesh_get16(p + 1);
    m->dest = p[3];
    m->type_prio = (uint8_t)(p[4] & (LMESH_MSG_TYPE_MASK | LMESH_MSG_PRIO_MASK | LMESH_MSG_FLAG_RESEND));
    m->resend = (p[4] & LMESH_MSG_FLAG_RESEND) != 0;
    m->lifetime_s = lmesh_get32(p + 5);
    m->hops = p[9];
    m->content = p + LMESH_MSG_HEADER_LEN;
    m->content_len = (uint8_t)(len - LMESH_MSG_HEADER_LEN);

    unsigned type = m->type_prio & LMESH_MSG_TYPE_MASK;
    unsigned prio = (m->type_prio & LMESH_MSG_PRIO_MASK) >> LMESH_MSG_PRIO_SHIFT;
    if (type > LMESH_MSG_RECEIPT || prio >= LMESH_PRIO_COUNT) {
        return false;
    }
    if (m->origin == LMESH_ID_NONE || m->origin == LMESH_ID_ALL || m->dest == LMESH_ID_NONE ||
        m->dest == LMESH_ID_ALL || m->origin == m->dest) {
        return false;
    }
    if (m->hops == 0 || m->lifetime_s == 0) {
        return false;
    }
    if (type == LMESH_MSG_RECEIPT && m->dest == node->ident.id &&
        (m->content_len < RECEIPT_FIXED_LEN || m->content_len > RECEIPT_FIXED_LEN + LMESH_RESULT_MAX ||
         m->content[2] > LMESH_RESULT_UNSUPPORTED)) {
        return false;
    }
    return true;
}

static void store_receipt(lmesh_node *node, const wire_msg *m, const lmesh_cmd_result *res)
{
    lmesh_prio prio = (lmesh_prio)((m->type_prio & LMESH_MSG_PRIO_MASK) >> LMESH_MSG_PRIO_SHIFT);
    lmesh_record *r = lmesh_store_alloc(node, prio, LMESH_ROLE_RELAY);
    if (!r) {
        return;
    }
    size_t room = LMESH_MAX_CONTENT - RECEIPT_FIXED_LEN;
    size_t extra = res->len < room ? res->len : room;

    r->origin = node->ident.id;
    r->dest = m->origin;
    r->id = lmesh_store_next_msg_id(node);
    r->type_prio = (uint8_t)(LMESH_MSG_RECEIPT | ((unsigned)prio << LMESH_MSG_PRIO_SHIFT));
    r->lifetime_s = RECEIPT_LIFETIME_S;
    r->hops = 0;
    lmesh_put16(r->content, m->id);
    r->content[2] = res->code;
    memcpy(r->content + RECEIPT_FIXED_LEN, res->data, extra);
    r->content_len = (uint8_t)(RECEIPT_FIXED_LEN + extra);
    r->submitted_ms = node->now_ms;
    r->next_attempt_ms = node->now_ms;
    lmesh_record_seal(r);
    lmesh_wake_at(node, node->now_ms);
}

static void take_receipt(lmesh_node *node, const wire_msg *m)
{
    uint16_t orig_id = lmesh_get16(m->content);
    uint8_t code = m->content[2];
    lmesh_record *r = lmesh_store_find(node, node->ident.id, orig_id, ORIGIN_ROLES);
    if (!r) {
        return;
    }
    uint16_t idx = index_of(node, r);
    if (node->msg.in_flight == (int)idx && lmesh_link_cancel(node, LMESH_OWNER_MESSAGE, idx)) {
        node->msg.in_flight = -1;
        lmesh_wake_at(node, node->now_ms);
    }
    r->in_flight_to = LMESH_ID_NONE;
    r->result_code = code;
    if (code == LMESH_RESULT_DELIVERED || code == LMESH_RESULT_EXECUTED) {
        node->stats.delivered++;
        lmesh_store_retire(node, r, LMESH_STATE_DELIVERED, LMESH_REASON_RECEIPT);
    } else {
        lmesh_store_retire(node, r, LMESH_STATE_FAILED, LMESH_REASON_REJECTED);
    }
}

static uint8_t deliver(lmesh_node *node, const wire_msg *m)
{
    lmesh_msg_type type = (lmesh_msg_type)(m->type_prio & LMESH_MSG_TYPE_MASK);
    lmesh_cmd_result res;
    memset(&res, 0, sizeof res);
    res.code = LMESH_RESULT_DELIVERED;

    if (lmesh_dedup_dest_accept(node, m->origin, m->id)) {
        lmesh_event ev = {.type = LMESH_EV_MSG_RECEIVED, .t_ms = node->now_ms};
        ev.u.msg_received.ref.origin = m->origin;
        ev.u.msg_received.ref.id = m->id;
        ev.u.msg_received.msg_type = type;
        ev.u.msg_received.prio = (lmesh_prio)((m->type_prio & LMESH_MSG_PRIO_MASK) >> LMESH_MSG_PRIO_SHIFT);
        ev.u.msg_received.hops = m->hops;
        ev.u.msg_received.len = m->content_len;
        ev.u.msg_received.data = m->content;
        lmesh_emit(node, &ev);
        if (type == LMESH_MSG_COMMAND) {
            if (node->cb.on_command) {
                res.code = LMESH_RESULT_EXECUTED;
                node->cb.on_command(node->cb.ctx, m->origin, m->content, m->content_len, &res);
                if (res.code > LMESH_RESULT_UNSUPPORTED) {
                    res.code = LMESH_RESULT_REJECTED;
                }
                if (res.len > LMESH_RESULT_MAX) {
                    res.len = LMESH_RESULT_MAX;
                }
            } else {
                res.code = LMESH_RESULT_UNSUPPORTED;
            }
            lmesh_dedup_remember_result(node, m->origin, m->id, res.code);
        }
        node->stats.delivered++;
        store_receipt(node, m, &res);
        return LMESH_ACK_ACCEPTED;
    }

    node->stats.dup_dest++;
    uint8_t code = LMESH_RESULT_DELIVERED;
    if (type == LMESH_MSG_COMMAND && lmesh_dedup_lookup_result(node, m->origin, m->id, &code)) {
        res.code = code;
    }
    store_receipt(node, m, &res);
    return LMESH_ACK_DUPLICATE;
}

static uint8_t relay(lmesh_node *node, const wire_msg *m)
{
    if (m->origin == node->ident.id || lmesh_store_find(node, m->origin, m->id, ACTIVE_ROLES) ||
        (!m->resend && lmesh_dedup_relay_seen(node, m->origin, m->id))) {
        node->stats.dup_relay++;
        return LMESH_ACK_DUPLICATE;
    }
    if (m->hops >= LMESH_MAX_HOPS) {
        node->stats.hop_limit_drops++;
        return LMESH_ACK_REFUSED_INVALID;
    }
    lmesh_prio prio = (lmesh_prio)((m->type_prio & LMESH_MSG_PRIO_MASK) >> LMESH_MSG_PRIO_SHIFT);
    lmesh_record *r = lmesh_store_alloc(node, prio, LMESH_ROLE_RELAY);
    if (!r) {
        return LMESH_ACK_REFUSED_FULL;
    }
    r->origin = m->origin;
    r->dest = m->dest;
    r->id = m->id;
    r->type_prio = m->type_prio;
    r->hops = m->hops;
    r->lifetime_s = m->lifetime_s;
    r->content_len = m->content_len;
    memcpy(r->content, m->content, m->content_len);
    r->submitted_ms = node->now_ms;
    r->next_attempt_ms = node->now_ms;
    lmesh_record_seal(r);
    lmesh_wake_at(node, node->now_ms);
    return LMESH_ACK_ACCEPTED;
}

void lmesh_message_rx_data(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len)
{
    if (!node || !hdr) {
        return;
    }
    wire_msg m;
    uint8_t status;

    if (!parse(node, payload, len, &m)) {
        node->stats.rx_malformed++;
        status = LMESH_ACK_REFUSED_INVALID;
    } else if (m.dest != node->ident.id) {
        status = relay(node, &m);
    } else if (m.hops > LMESH_MAX_HOPS) {
        node->stats.hop_limit_drops++;
        status = LMESH_ACK_REFUSED_INVALID;
    } else if ((m.type_prio & LMESH_MSG_TYPE_MASK) == LMESH_MSG_RECEIPT) {
        if (lmesh_dedup_dest_accept(node, m.origin, m.id)) {
            take_receipt(node, &m);
            status = LMESH_ACK_ACCEPTED;
        } else {
            node->stats.dup_dest++;
            status = LMESH_ACK_DUPLICATE;
        }
    } else {
        status = deliver(node, &m);
    }
    lmesh_link_send_ack(node, hdr->tx, hdr->seq, status);
}

static void custody_passed(lmesh_node *node, lmesh_record *r)
{
    if (r->role == LMESH_ROLE_RELAY) {
        if (r->origin != node->ident.id) {
            lmesh_dedup_relay_remember(node, r->origin, r->id);
        }
        lmesh_store_release(node, r);
        return;
    }
    bool first = r->role == LMESH_ROLE_ORIGIN_COPY || r->e2e_backoff_ms == 0;
    r->role = LMESH_ROLE_ORIGIN_SHADOW;
    r->state = LMESH_STATE_TRANSMITTED;
    r->e2e_backoff_ms = first ? LMESH_E2E_RETRY_MIN_MS : doubled(r->e2e_backoff_ms, LMESH_E2E_RETRY_MAX_MS);
    r->next_attempt_ms = node->now_ms + r->e2e_backoff_ms;
    lmesh_record_seal(r);
    emit_state(node, r, LMESH_REASON_HOP_ACCEPTED);
    lmesh_wake_at(node, r->next_attempt_ms);
}

void lmesh_message_link_result(lmesh_node *node, uint16_t record_index, uint8_t ack_status)
{
    if (!node || record_index >= LMESH_STORE_SIZE || node->msg.in_flight != (int)record_index) {
        return;
    }
    node->msg.in_flight = -1;
    lmesh_wake_at(node, node->now_ms);

    lmesh_record *r = &node->store.records[record_index];
    if (!schedulable(r) || r->in_flight_to == LMESH_ID_NONE || !lmesh_record_valid(r)) {
        return;
    }
    r->in_flight_to = LMESH_ID_NONE;

    switch (ack_status) {
    case LMESH_ACK_ACCEPTED:
    case LMESH_ACK_DUPLICATE:
        custody_passed(node, r);
        break;
    case LMESH_ACK_REFUSED_FULL:
        r->next_attempt_ms = node->now_ms + LMESH_REFUSE_BACKOFF_MS;
        lmesh_record_seal(r);
        lmesh_wake_at(node, r->next_attempt_ms);
        break;
    case LMESH_ACK_REFUSED_INVALID:
        if (r->role == LMESH_ROLE_RELAY) {
            lmesh_store_release(node, r);
        } else {
            lmesh_store_retire(node, r, LMESH_STATE_FAILED, LMESH_REASON_INVALID);
        }
        break;
    default:
        if (r->attempts < UINT8_MAX) {
            r->attempts++;
        }
        r->next_attempt_ms = node->now_ms + fwd_backoff(node, r->attempts);
        lmesh_record_seal(r);
        lmesh_wake_at(node, r->next_attempt_ms);
        break;
    }
}

void lmesh_message_route_event(lmesh_node *node, lmesh_node_id dest)
{
    if (!node) {
        return;
    }
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        lmesh_record *r = &node->store.records[i];
        if (!schedulable(r) || r->dest != dest || (int)i == node->msg.in_flight || !lmesh_record_valid(r)) {
            continue;
        }
        if (lmesh_time_reached(node->now_ms, r->next_attempt_ms) && r->next_attempt_ms != node->now_ms) {
            r->next_attempt_ms = node->now_ms;
            lmesh_record_seal(r);
        }
    }
    lmesh_wake_at(node, node->now_ms);
}

static void check_in_flight(lmesh_node *node)
{
    int idx = node->msg.in_flight;
    if (idx < 0) {
        return;
    }
    if (idx >= LMESH_STORE_SIZE) {
        node->msg.in_flight = -1;
        return;
    }
    lmesh_record *r = &node->store.records[idx];
    bool alive = schedulable(r) && r->in_flight_to != LMESH_ID_NONE && lmesh_record_valid(r);
    if (!alive) {
        (void)lmesh_link_cancel(node, LMESH_OWNER_MESSAGE, (uint16_t)idx);
    }
    if (!link_holds(node, (uint16_t)idx)) {
        node->msg.in_flight = -1;
        if (alive) {
            r->in_flight_to = LMESH_ID_NONE;
            lmesh_record_seal(r);
        }
    }
}

static bool hand_over(lmesh_node *node, uint16_t idx)
{
    lmesh_record *r = &node->store.records[idx];
    lmesh_node_id hop = next_hop(node, r->dest);
    uint8_t buf[LMESH_MAX_PAYLOAD];
    size_t len = encode(node, r, buf);

    if (lmesh_link_send(node, LMESH_FT_DATA, hop, buf, len, (uint8_t)lmesh_record_prio(r), node->now_ms, true,
                        LMESH_OWNER_MESSAGE, idx) != LMESH_OK) {
        lmesh_wake_at(node, node->now_ms + lmesh_link_slot_ms(node));
        return false;
    }
    node->msg.in_flight = (int8_t)idx;
    r->in_flight_to = hop;
    bool resend = r->role == LMESH_ROLE_ORIGIN_SHADOW && r->state == LMESH_STATE_TRANSMITTED;
    if (resend) {
        r->state = LMESH_STATE_QUEUED;
    }
    lmesh_record_seal(r);
    if (resend) {
        emit_state(node, r, LMESH_REASON_RESEND);
    }
    return true;
}

void lmesh_message_tick(lmesh_node *node)
{
    uint32_t now = node->now_ms;
    int best[LMESH_PRIO_COUNT];

    check_in_flight(node);
    for (unsigned b = 0; b < LMESH_PRIO_COUNT; b++) {
        best[b] = -1;
    }
    for (int i = 0; i < LMESH_STORE_SIZE; i++) {
        lmesh_record *r = &node->store.records[i];
        if (!schedulable(r) || i == node->msg.in_flight || !lmesh_record_valid(r)) {
            continue;
        }
        if (r->in_flight_to != LMESH_ID_NONE) {
            r->in_flight_to = LMESH_ID_NONE;
            lmesh_record_seal(r);
        }
        if (!lmesh_time_reached(now, r->next_attempt_ms)) {
            lmesh_wake_at(node, r->next_attempt_ms);
            continue;
        }
        if (lmesh_record_remaining_s(node, r) == 0) {
            continue;
        }
        if (next_hop(node, r->dest) == LMESH_ID_NONE) {
            if (now - r->next_attempt_ms >= STALE_DUE_MS) {
                r->next_attempt_ms = now;
                lmesh_record_seal(r);
            }
            continue;
        }
        unsigned p = lmesh_record_prio(r);
        if (p >= LMESH_PRIO_COUNT) {
            continue;
        }
        if (best[p] < 0 || (int32_t)(r->submitted_ms - node->store.records[best[p]].submitted_ms) < 0) {
            best[p] = i;
        }
    }
    if (node->msg.in_flight >= 0) {
        return;
    }

    uint8_t *credit = node->msg.wrr_credit;
    unsigned start = 0;
    while (start < LMESH_PRIO_COUNT && credit[start] == 0) {
        start++;
    }
    int chosen = -1;
    unsigned band = 0;
    bool new_round = false;
    for (unsigned b = start; b < LMESH_PRIO_COUNT && chosen < 0; b++) {
        if (best[b] >= 0) {
            chosen = best[b];
            band = b;
        }
    }
    for (unsigned b = 0; b < LMESH_PRIO_COUNT && chosen < 0; b++) {
        if (best[b] >= 0) {
            chosen = best[b];
            band = b;
            new_round = true;
        }
    }
    if (chosen < 0 || !hand_over(node, (uint16_t)chosen)) {
        return;
    }
    if (new_round) {
        memcpy(credit, wrr_weight, sizeof wrr_weight);
    }
    for (unsigned b = 0; b < band; b++) {
        credit[b] = 0;
    }
    if (credit[band] > 0) {
        credit[band]--;
    }
}
