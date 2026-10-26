/* Route table: feasibility condition, selection with hysteresis, retraction, expiry, sequence-number requests. */
#include <string.h>

#include "flood.h"
#include "hello.h"
#include "message.h"
#include "neighbor.h"
#include "route.h"

#define SEQNO_SLOT_LEN 4u
#define METRIC_MAX_FINITE 254u
#define SWITCH_EVALUATIONS 2u

static size_t slot_of(const lmesh_node *node, const lmesh_route *r)
{
    return (size_t)(r - node->route.table);
}

static void save_seqno(lmesh_node *node)
{
    if (!node->port.storage_write) {
        return;
    }
    uint8_t b[SEQNO_SLOT_LEN];
    lmesh_put16(b, node->route.own_seqno);
    lmesh_put16(b + 2, (uint16_t)~node->route.own_seqno);
    (void)node->port.storage_write(node->port.ctx, LMESH_SLOT_SEQNO, b, sizeof b);
}

void lmesh_route_init(lmesh_node *node)
{
    uint8_t b[SEQNO_SLOT_LEN];
    if (node->port.storage_read && node->port.storage_read(node->port.ctx, LMESH_SLOT_SEQNO, b, sizeof b) == 0 &&
        lmesh_get16(b + 2) == (uint16_t)~lmesh_get16(b)) {
        node->route.own_seqno = (uint16_t)(lmesh_get16(b) + LMESH_SEQNO_BOOT_MARGIN);
    } else {
        node->route.own_seqno = (uint16_t)lmesh_rand_range(node, 0, 0xFFFFu);
    }
    save_seqno(node);
    lmesh_hello_init(node);
}

lmesh_route *lmesh_route_find(lmesh_node *node, lmesh_node_id dest)
{
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        lmesh_route *r = &node->route.table[i];
        if (r->used && r->dest == dest) {
            return r;
        }
    }
    return NULL;
}

lmesh_node_id lmesh_route_next_hop(const lmesh_node *node, lmesh_node_id dest)
{
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        const lmesh_route *r = &node->route.table[i];
        if (r->used && r->dest == dest) {
            return r->metric8 == LMESH_METRIC_INF ? LMESH_ID_NONE : r->next_hop;
        }
    }
    return LMESH_ID_NONE;
}

bool lmesh_route_feasible(const lmesh_route *r, uint16_t seqno, uint8_t metric8)
{
    if (metric8 == LMESH_METRIC_INF || !r->fd_valid) {
        return true;
    }
    if (lmesh_seqno_newer(seqno, r->fd_seqno)) {
        return true;
    }
    return seqno == r->fd_seqno && metric8 < r->fd_metric8;
}

void lmesh_route_request_body(const lmesh_route *r, uint8_t body[LMESH_ROUTE_REQ_BODY_LEN])
{
    body[0] = r->dest;
    lmesh_put16(body + 1, (uint16_t)(r->fd_seqno + 1u));
}

static uint8_t add_metric(uint8_t a, uint8_t b)
{
    if (a == LMESH_METRIC_INF || b == LMESH_METRIC_INF) {
        return LMESH_METRIC_INF;
    }
    unsigned sum = (unsigned)a + b;
    return sum > METRIC_MAX_FINITE ? (uint8_t)METRIC_MAX_FINITE : (uint8_t)sum;
}

static bool adv_live(const lmesh_node *node, const lmesh_adv *a)
{
    return a->valid && a->metric8 != LMESH_METRIC_INF &&
           !lmesh_time_reached(node->now_ms, a->rx_ms + LMESH_ROUTE_HOLD_MS);
}

static void lower_fd(lmesh_route *r)
{
    if (!r->fd_valid || lmesh_seqno_newer(r->seqno, r->fd_seqno) ||
        (r->seqno == r->fd_seqno && r->metric8 < r->fd_metric8)) {
        r->fd_valid = true;
        r->fd_seqno = r->seqno;
        r->fd_metric8 = r->metric8;
    }
}

static void emit_route(lmesh_node *node, lmesh_event_type type, const lmesh_route *r)
{
    lmesh_event ev = {.type = type, .t_ms = node->now_ms};
    ev.u.route.dest = r->dest;
    ev.u.route.next_hop = r->next_hop;
    ev.u.route.metric8 = r->metric8;
    ev.u.route.seqno = r->seqno;
    lmesh_emit(node, &ev);
}

static void maybe_request(lmesh_node *node, lmesh_route *r)
{
    if (!r->fd_valid || !lmesh_time_reached(node->now_ms, r->last_req_ms + LMESH_ROUTE_REQ_GAP_MS)) {
        return;
    }
    uint8_t body[LMESH_ROUTE_REQ_BODY_LEN];
    lmesh_route_request_body(r, body);
    r->last_req_ms = node->now_ms;
    (void)lmesh_flood_originate(node, LMESH_FT_ROUTE_REQ, body, sizeof body);
}

static void select_route(lmesh_node *node, lmesh_route *r)
{
    size_t slot = slot_of(node, r);
    bool have_current = r->metric8 != LMESH_METRIC_INF && r->next_hop != LMESH_ID_NONE;
    const lmesh_neighbor *best = NULL;
    const lmesh_neighbor *cur = NULL;
    uint8_t best_cost = LMESH_METRIC_INF;
    uint8_t cur_cost = LMESH_METRIC_INF;
    bool infeasible = false;

    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        const lmesh_neighbor *n = &node->nbr.table[i];
        if (!n->used || !n->up || !n->bidirectional) {
            continue;
        }
        const lmesh_adv *a = &n->adv[slot];
        if (!adv_live(node, a)) {
            continue;
        }
        if (!lmesh_route_feasible(r, a->seqno, a->metric8)) {
            infeasible = true;
            continue;
        }
        uint8_t cost = add_metric(a->metric8, n->etx8);
        if (cost == LMESH_METRIC_INF) {
            continue;
        }
        bool is_cur = have_current && n->id == r->next_hop;
        if (is_cur) {
            cur = n;
            cur_cost = cost;
        }
        if (cost < best_cost || (cost == best_cost && is_cur)) {
            best = n;
            best_cost = cost;
        }
    }

    if (cur && best != cur) {
        bool better = (unsigned)best_cost + LMESH_ROUTE_HYSTERESIS <= cur_cost;
        if (better && r->better_count + 1u >= SWITCH_EVALUATIONS) {
            r->better_count = 0;
        } else {
            r->better_count = better ? (uint8_t)(r->better_count + 1u) : 0u;
            best = cur;
            best_cost = cur_cost;
        }
    } else {
        r->better_count = 0;
    }

    if (!best) {
        if (r->metric8 != LMESH_METRIC_INF) {
            r->metric8 = LMESH_METRIC_INF;
            r->next_hop = LMESH_ID_NONE;
            r->retract_left = LMESH_RETRACT_REPEATS;
            emit_route(node, LMESH_EV_ROUTE_LOST, r);
            lmesh_message_route_event(node, r->dest);
            lmesh_hello_trigger(node);
        }
        if (infeasible) {
            maybe_request(node, r);
        }
        return;
    }

    const lmesh_adv *a = &best->adv[slot];
    bool was_lost = r->metric8 == LMESH_METRIC_INF;
    bool new_hop = best->id != r->next_hop;
    bool new_seqno = a->seqno != r->seqno;
    bool changed = was_lost || new_hop || new_seqno || best_cost != r->metric8;

    r->next_hop = best->id;
    r->metric8 = best_cost;
    r->seqno = a->seqno;
    r->refreshed_ms = a->rx_ms;
    r->retract_left = 0;
    lower_fd(r);

    if (changed) {
        emit_route(node, LMESH_EV_ROUTE_CHANGED, r);
        lmesh_message_route_event(node, r->dest);
    }
    if (was_lost || new_hop || new_seqno) {
        lmesh_hello_trigger(node);
    }
}

static bool slot_reclaimable(const lmesh_node *node, size_t slot)
{
    const lmesh_route *r = &node->route.table[slot];
    if (r->metric8 != LMESH_METRIC_INF || r->retract_left > 0) {
        return false;
    }
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        const lmesh_neighbor *n = &node->nbr.table[i];
        if (n->used && n->adv[slot].valid) {
            return false;
        }
    }
    return true;
}

static lmesh_route *slot_alloc(lmesh_node *node, lmesh_node_id dest)
{
    size_t slot = LMESH_MAX_NODES;
    for (size_t i = 0; i < LMESH_MAX_NODES && slot == LMESH_MAX_NODES; i++) {
        if (!node->route.table[i].used) {
            slot = i;
        }
    }
    for (size_t i = 0; i < LMESH_MAX_NODES && slot == LMESH_MAX_NODES; i++) {
        if (slot_reclaimable(node, i)) {
            slot = i;
        }
    }
    if (slot == LMESH_MAX_NODES) {
        return NULL;
    }
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        memset(&node->nbr.table[i].adv[slot], 0, sizeof(lmesh_adv));
    }
    lmesh_route *r = &node->route.table[slot];
    memset(r, 0, sizeof *r);
    r->used = true;
    r->dest = dest;
    r->next_hop = LMESH_ID_NONE;
    r->metric8 = LMESH_METRIC_INF;
    r->last_req_ms = node->now_ms - LMESH_ROUTE_REQ_GAP_MS;
    return r;
}

void lmesh_route_update(lmesh_node *node, lmesh_neighbor *from, lmesh_node_id dest, uint16_t seqno, uint8_t metric8)
{
    if (!from || !from->used || dest == LMESH_ID_NONE || dest == LMESH_ID_ALL || dest == node->ident.id) {
        return;
    }
    lmesh_route *r = lmesh_route_find(node, dest);
    if (!r) {
        if (metric8 == LMESH_METRIC_INF) {
            return;
        }
        r = slot_alloc(node, dest);
        if (!r) {
            return;
        }
    }
    lmesh_adv *a = &from->adv[slot_of(node, r)];
    a->valid = true;
    a->seqno = seqno;
    a->metric8 = metric8;
    a->rx_ms = node->now_ms;
    select_route(node, r);
}

void lmesh_route_neighbor_down(lmesh_node *node, lmesh_node_id id)
{
    lmesh_neighbor *n = lmesh_neighbor_find(node, id);
    if (n) {
        for (size_t s = 0; s < LMESH_MAX_NODES; s++) {
            n->adv[s].valid = false;
        }
    }
    for (size_t s = 0; s < LMESH_MAX_NODES; s++) {
        if (node->route.table[s].used) {
            select_route(node, &node->route.table[s]);
        }
    }
}

void lmesh_route_rx_request(lmesh_node *node, lmesh_node_id dest, uint16_t seqno)
{
    if (dest != node->ident.id || !lmesh_seqno_newer(seqno, node->route.own_seqno)) {
        return;
    }
    node->route.own_seqno = seqno;
    save_seqno(node);
    lmesh_hello_trigger(node);
}

void lmesh_route_tick(lmesh_node *node)
{
    bool affected[LMESH_MAX_NODES] = {false};

    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        lmesh_neighbor *n = &node->nbr.table[i];
        if (!n->used) {
            continue;
        }
        for (size_t s = 0; s < LMESH_MAX_NODES; s++) {
            lmesh_adv *a = &n->adv[s];
            if (!a->valid) {
                continue;
            }
            uint32_t expiry = a->rx_ms + LMESH_ROUTE_HOLD_MS;
            if (lmesh_time_reached(node->now_ms, expiry)) {
                a->valid = false;
                affected[s] = true;
            } else {
                lmesh_wake_at(node, expiry);
            }
        }
    }
    for (size_t s = 0; s < LMESH_MAX_NODES; s++) {
        lmesh_route *r = &node->route.table[s];
        if (!r->used) {
            continue;
        }
        if (lmesh_time_reached(node->now_ms, r->last_req_ms + LMESH_ROUTE_REQ_GAP_MS)) {
            r->last_req_ms = node->now_ms - LMESH_ROUTE_REQ_GAP_MS;
        }
        if (affected[s]) {
            select_route(node, r);
        }
    }
}
