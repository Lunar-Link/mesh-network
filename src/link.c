/* Transmit queue with weighted round-robin bands, hop acknowledgment, retries, repeat detection. SPEC.md Section 6. */
#include "link.h"

#include <string.h>

#include "message.h"
#include "neighbor.h"

_Static_assert(LMESH_WRR_URGENT >= 1 && LMESH_WRR_URGENT <= 255, "WRR weights are 1 to 255");
_Static_assert(LMESH_WRR_NORMAL >= 1 && LMESH_WRR_NORMAL <= 255, "WRR weights are 1 to 255");
_Static_assert(LMESH_WRR_ROUTINE >= 1 && LMESH_WRR_ROUTINE <= 255, "WRR weights are 1 to 255");
_Static_assert(LMESH_HOP_RETRIES <= 16, "backoff window is 2^attempts slots");
_Static_assert(LMESH_MAX_FRAME <= 255, "frame length is one byte");

static const uint8_t wrr_weight[LMESH_PRIO_COUNT] = {LMESH_WRR_URGENT, LMESH_WRR_NORMAL, LMESH_WRR_ROUTINE};

static uint32_t clock_now(const lmesh_node *node)
{
    return node->port.now_ms(node->port.ctx);
}

uint32_t lmesh_link_slot_ms(const lmesh_node *node)
{
    return lmesh_airtime(node, LMESH_MAX_FRAME);
}

static void report(lmesh_node *node, uint8_t owner, uint16_t owner_ref, uint8_t status)
{
    if (owner == LMESH_OWNER_MESSAGE) {
        lmesh_message_link_result(node, owner_ref, status);
    }
}

static bool radio_tx(lmesh_node *node, const uint8_t *buf, size_t len)
{
    if (node->port.radio_send(node->port.ctx, buf, len) != 0) {
        return false;
    }
    node->link.tx_active = true;
    node->stats.tx_frames[buf[0] & 0x0Fu]++;
    node->stats.airtime_ms += lmesh_airtime(node, len);
    return true;
}

static void finish(lmesh_node *node, int idx, uint8_t status, bool link_ok, bool tell_neighbor)
{
    lmesh_link_state *l = &node->link;
    lmesh_txq_entry *e = &l->queue[idx];
    uint8_t owner = e->owner;
    uint16_t owner_ref = e->owner_ref;
    lmesh_node_id to = e->frame.buf[3];

    memset(e, 0, sizeof *e);
    if (l->in_flight == idx) {
        l->in_flight = -1;
        l->awaiting_ack = false;
    }
    if (tell_neighbor) {
        lmesh_neighbor_link_result(node, to, link_ok);
    }
    report(node, owner, owner_ref, status);
}

static void attempt_failed(lmesh_node *node, uint32_t now)
{
    lmesh_link_state *l = &node->link;
    int idx = l->in_flight;
    lmesh_txq_entry *e = &l->queue[idx];

    l->in_flight = -1;
    l->awaiting_ack = false;
    node->stats.ack_timeouts++;
    e->attempts++;
    if (e->attempts <= LMESH_HOP_RETRIES) {
        e->frame.buf[5] = (uint8_t)(e->frame.buf[5] | LMESH_FLAG_RETRY);
        lmesh_frame_retag(node, e->frame.buf, e->frame.len);
        e->due_ms = now + lmesh_rand_range(node, 1u, 1u << e->attempts) * lmesh_link_slot_ms(node);
        lmesh_wake_at(node, e->due_ms);
        return;
    }
    node->stats.link_failures++;
    finish(node, idx, LMESH_ACK_TIMEOUT, false, true);
}

static int pick(const lmesh_node *node, uint8_t *band_out, bool *new_round)
{
    const lmesh_link_state *l = &node->link;
    int best[LMESH_PRIO_COUNT];

    for (unsigned b = 0; b < LMESH_PRIO_COUNT; b++) {
        best[b] = -1;
    }
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        const lmesh_txq_entry *e = &l->queue[i];
        if (!e->used || i == l->in_flight || !lmesh_time_reached(node->now_ms, e->due_ms)) {
            continue;
        }
        if (best[e->band] < 0 || (int32_t)(e->due_ms - l->queue[best[e->band]].due_ms) < 0) {
            best[e->band] = i;
        }
    }

    unsigned start = 0;
    while (start < LMESH_PRIO_COUNT && l->wrr_credit[start] == 0) {
        start++;
    }
    for (unsigned b = start; b < LMESH_PRIO_COUNT; b++) {
        if (best[b] >= 0) {
            *band_out = (uint8_t)b;
            *new_round = false;
            return best[b];
        }
    }
    for (unsigned b = 0; b < LMESH_PRIO_COUNT; b++) {
        if (best[b] >= 0) {
            *band_out = (uint8_t)b;
            *new_round = true;
            return best[b];
        }
    }
    return -1;
}

static void wrr_commit(lmesh_link_state *l, uint8_t band, bool new_round)
{
    if (new_round) {
        memcpy(l->wrr_credit, wrr_weight, sizeof l->wrr_credit);
    }
    for (unsigned b = 0; b < band; b++) {
        l->wrr_credit[b] = 0;
    }
    l->wrr_credit[band]--;
}

static void wake_for_queue(lmesh_node *node)
{
    const lmesh_link_state *l = &node->link;
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        if (l->queue[i].used && i != l->in_flight) {
            lmesh_wake_at(node, l->queue[i].due_ms);
        }
    }
}

static bool send_pending_ack(lmesh_node *node)
{
    lmesh_link_state *l = &node->link;
    uint8_t payload[2] = {l->ack_seq, l->ack_status};
    lmesh_frame_hdr h = {
        .type = LMESH_FT_HOP_ACK, .tx = node->ident.id, .rx = l->ack_to, .seq = l->frame_seq, .payload_len = 2};

    size_t n = lmesh_frame_encode(node, &h, payload, l->ack_buf, sizeof l->ack_buf);
    if (n == 0 || !radio_tx(node, l->ack_buf, n)) {
        return false;
    }
    l->frame_seq++;
    l->ack_pending = false;
    l->ack_on_air = true;
    return true;
}

static void forget_rx(lmesh_link_state *l, lmesh_node_id id)
{
    const size_t n = LMESH_MAX_NEIGHBORS;

    for (size_t i = 0; i < n; i++) {
        if (l->last_rx[i].id != id) {
            continue;
        }
        for (; i + 1 < n; i++) {
            l->last_rx[i] = l->last_rx[i + 1];
        }
        l->last_rx[n - 1].id = LMESH_ID_NONE;
        l->last_rx[n - 1].last_seq = 0;
        return;
    }
}

static void remember_rx(lmesh_link_state *l, lmesh_node_id id, uint8_t seq)
{
    const size_t n = LMESH_MAX_NEIGHBORS;
    size_t i = 0;

    forget_rx(l, id);
    while (i < n && l->last_rx[i].id != LMESH_ID_NONE) {
        i++;
    }
    if (i == n) {
        for (i = 0; i + 1 < n; i++) {
            l->last_rx[i] = l->last_rx[i + 1];
        }
    }
    l->last_rx[i].id = id;
    l->last_rx[i].last_seq = seq;
}

lmesh_err lmesh_link_send(lmesh_node *node, uint8_t type, lmesh_node_id to, const uint8_t *payload, size_t len,
                          uint8_t band, uint32_t due_ms, bool want_ack, uint8_t owner, uint16_t owner_ref)
{
    if (node == NULL || type > 15u || band >= LMESH_PRIO_COUNT || (len > 0 && payload == NULL) ||
        (want_ack && to == LMESH_ID_ALL)) {
        return LMESH_ERR_ARG;
    }
    if (len > LMESH_MAX_PAYLOAD) {
        return LMESH_ERR_TOO_LONG;
    }

    lmesh_link_state *l = &node->link;
    lmesh_txq_entry *e = NULL;
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        if (!l->queue[i].used) {
            e = &l->queue[i];
            break;
        }
    }
    if (e == NULL) {
        return LMESH_ERR_FULL;
    }

    lmesh_frame_hdr h = {.type = type,
                         .tx = node->ident.id,
                         .rx = to,
                         .seq = l->frame_seq,
                         .flags = want_ack ? (uint8_t)LMESH_FLAG_ACK_REQ : 0u,
                         .payload_len = (uint8_t)len};
    size_t n = lmesh_frame_encode(node, &h, payload, e->frame.buf, sizeof e->frame.buf);
    if (n == 0) {
        return LMESH_ERR_ARG;
    }
    l->frame_seq++;
    e->used = true;
    e->want_ack = want_ack;
    e->band = band;
    e->attempts = 0;
    e->owner = owner;
    e->owner_ref = owner_ref;
    e->due_ms = due_ms;
    e->frame.len = (uint8_t)n;
    lmesh_wake_at(node, due_ms);
    return LMESH_OK;
}

bool lmesh_link_cancel(lmesh_node *node, uint8_t owner, uint16_t owner_ref)
{
    lmesh_link_state *l = &node->link;
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        lmesh_txq_entry *e = &l->queue[i];
        if (e->used && i != l->in_flight && e->owner == owner && e->owner_ref == owner_ref) {
            memset(e, 0, sizeof *e);
            return true;
        }
    }
    return false;
}

void lmesh_link_send_ack(lmesh_node *node, lmesh_node_id to, uint8_t seq, uint8_t status)
{
    lmesh_link_state *l = &node->link;

    if (status == LMESH_ACK_ACCEPTED || status == LMESH_ACK_DUPLICATE) {
        remember_rx(l, to, seq);
    }
    l->ack_pending = true;
    l->ack_to = to;
    l->ack_seq = seq;
    l->ack_status = status;
    if (!l->tx_active && !send_pending_ack(node)) {
        lmesh_wake_at(node, clock_now(node));
    }
}

bool lmesh_link_is_repeat(lmesh_node *node, const lmesh_frame_hdr *hdr)
{
    if ((hdr->flags & LMESH_FLAG_RETRY) == 0) {
        return false;
    }
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        if (node->link.last_rx[i].id == hdr->tx) {
            return node->link.last_rx[i].last_seq == hdr->seq;
        }
    }
    return false;
}

void lmesh_link_rx_ack(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len)
{
    lmesh_link_state *l = &node->link;

    if (hdr == NULL || payload == NULL || len < 2 || l->in_flight < 0 || !l->awaiting_ack ||
        payload[1] > LMESH_ACK_REFUSED_INVALID) {
        return;
    }
    const lmesh_txq_entry *e = &l->queue[l->in_flight];
    if (hdr->tx != e->frame.buf[3] || payload[0] != e->frame.buf[4]) {
        return;
    }
    finish(node, l->in_flight, payload[1], true, true);
    lmesh_wake_at(node, clock_now(node));
}

void lmesh_link_tx_done(lmesh_node *node, bool ok)
{
    lmesh_link_state *l = &node->link;

    if (!l->tx_active) {
        return;
    }
    l->tx_active = false;
    uint32_t now = clock_now(node);
    lmesh_wake_at(node, now);
    if (l->ack_on_air) {
        l->ack_on_air = false;
        return;
    }
    if (l->in_flight < 0 || l->awaiting_ack) {
        return;
    }

    const lmesh_txq_entry *e = &l->queue[l->in_flight];
    if (!e->want_ack) {
        finish(node, l->in_flight, ok ? (uint8_t)LMESH_ACK_ACCEPTED : (uint8_t)LMESH_ACK_TIMEOUT, ok, false);
    } else if (ok) {
        l->awaiting_ack = true;
        l->ack_deadline_ms = now + lmesh_airtime(node, e->frame.len) + lmesh_airtime(node, LMESH_HOP_ACK_FRAME_LEN) +
                             LMESH_ACK_GUARD_MS;
        lmesh_wake_at(node, l->ack_deadline_ms);
    } else {
        attempt_failed(node, now);
    }
}

void lmesh_link_neighbor_reset(lmesh_node *node, lmesh_node_id id)
{
    forget_rx(&node->link, id);
}

void lmesh_link_tick(lmesh_node *node)
{
    lmesh_link_state *l = &node->link;
    uint32_t now = node->now_ms;

    if (l->in_flight >= 0 && l->awaiting_ack) {
        if (lmesh_time_reached(now, l->ack_deadline_ms)) {
            attempt_failed(node, now);
        } else {
            lmesh_wake_at(node, l->ack_deadline_ms);
        }
    }
    if (l->tx_active) {
        return;
    }
    if (l->ack_pending && !send_pending_ack(node)) {
        lmesh_wake_at(node, now + 1u);
        return;
    }
    if (l->tx_active || l->in_flight >= 0) {
        return;
    }

    uint8_t band = 0;
    bool new_round = false;
    int idx = pick(node, &band, &new_round);
    if (idx >= 0) {
        lmesh_txq_entry *e = &l->queue[idx];
        if (node->port.radio_busy != NULL && node->port.radio_busy(node->port.ctx)) {
            e->due_ms = now + lmesh_rand_range(node, 1u, 4u) * lmesh_link_slot_ms(node);
        } else if (radio_tx(node, e->frame.buf, e->frame.len)) {
            wrr_commit(l, band, new_round);
            l->in_flight = (int8_t)idx;
            return;
        } else {
            lmesh_wake_at(node, now + 1u);
            return;
        }
    }
    wake_for_queue(node);
}
