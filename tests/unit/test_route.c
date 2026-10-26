/* Unit tests for the route table: feasibility, selection, hysteresis, retraction, expiry, requests, own seqno. */
#include <stdio.h>
#include <string.h>

#include "hello.h"
#include "neighbor.h"
#include "route.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(cond)) {                                                                        \
            failures++;                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                     \
    } while (0)

#define MAX_EVENTS 64

static uint32_t clock_ms;
static uint32_t rnd_value = 4;
static lmesh_event events[MAX_EVENTS];
static int n_events;
static uint8_t seqno_slot[4];
static bool seqno_stored;
static int seqno_writes;

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    return rnd_value;
}

static int radio_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    (void)frame;
    (void)len;
    return 0;
}

static uint32_t airtime_ms(void *ctx, size_t len)
{
    (void)ctx;
    return 15u + (uint32_t)len * 2u;
}

static int storage_read(void *ctx, uint8_t slot, void *buf, size_t len)
{
    (void)ctx;
    if (slot != LMESH_SLOT_SEQNO || !seqno_stored || len != sizeof seqno_slot) {
        return -1;
    }
    memcpy(buf, seqno_slot, len);
    return 0;
}

static int storage_write(void *ctx, uint8_t slot, const void *buf, size_t len)
{
    (void)ctx;
    if (slot != LMESH_SLOT_SEQNO) {
        return 0;
    }
    if (len != sizeof seqno_slot) {
        return -1;
    }
    memcpy(seqno_slot, buf, len);
    seqno_stored = true;
    seqno_writes++;
    return 0;
}

static void on_event(void *ctx, const lmesh_event *ev)
{
    (void)ctx;
    if (n_events < MAX_EVENTS) {
        events[n_events] = *ev;
    }
    n_events++;
}

static lmesh_node node;

static void setup_with(lmesh_node_id id, bool storage)
{
    lmesh_identity ident = {.id = id, .network_id = 1};
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    if (storage) {
        port.storage_read = storage_read;
        port.storage_write = storage_write;
    }
    lmesh_callbacks cb = {.on_event = on_event};
    clock_ms = 1000;
    CHECK(lmesh_init(&node, &ident, &port, &cb) == LMESH_OK);
    n_events = 0;
}

static void setup(lmesh_node_id id)
{
    setup_with(id, false);
}

static lmesh_neighbor *usable(lmesh_node_id id, uint8_t etx8)
{
    lmesh_neighbor *n = lmesh_neighbor_touch(&node, id, -80, 0);
    n->up = true;
    n->bidirectional = true;
    n->etx8 = etx8;
    n->last_hello_ms = node.now_ms;
    return n;
}

static bool route_event(int i, lmesh_event_type type, lmesh_node_id dest, lmesh_node_id next, uint8_t metric8,
                        uint16_t seqno)
{
    if (i >= n_events || i >= MAX_EVENTS) {
        return false;
    }
    const lmesh_event *e = &events[i];
    return e->type == type && e->u.route.dest == dest && e->u.route.next_hop == next &&
           e->u.route.metric8 == metric8 && e->u.route.seqno == seqno;
}

static void test_feasibility(void)
{
    lmesh_route r;
    memset(&r, 0, sizeof r);
    r.dest = 9;
    CHECK(lmesh_route_feasible(&r, 1, 100));
    CHECK(lmesh_route_feasible(&r, 1, 254));

    r.fd_valid = true;
    r.fd_seqno = 10;
    r.fd_metric8 = 20;
    CHECK(lmesh_route_feasible(&r, 10, LMESH_METRIC_INF));
    CHECK(lmesh_route_feasible(&r, 3, LMESH_METRIC_INF));
    CHECK(lmesh_route_feasible(&r, 11, 200));
    CHECK(!lmesh_route_feasible(&r, 9, 1));
    CHECK(lmesh_route_feasible(&r, 10, 19));
    CHECK(!lmesh_route_feasible(&r, 10, 20));
    CHECK(!lmesh_route_feasible(&r, 10, 21));

    r.fd_seqno = 0xFFFF;
    CHECK(lmesh_route_feasible(&r, 0x0000, 200));
    CHECK(!lmesh_route_feasible(&r, 0xFFFE, 0));

    uint8_t body[LMESH_ROUTE_REQ_BODY_LEN];
    lmesh_route_request_body(&r, body);
    CHECK(body[0] == 9 && body[1] == 0 && body[2] == 0);
    r.fd_seqno = 0x01FF;
    lmesh_route_request_body(&r, body);
    CHECK(body[0] == 9 && lmesh_get16(body + 1) == 0x0200);
}

static void test_selection_and_requests(void)
{
    setup(1);
    lmesh_neighbor *n2 = usable(2, 8);
    lmesh_neighbor *n3 = usable(3, 8);
    lmesh_neighbor *n4 = usable(4, 8);

    lmesh_route_update(&node, n2, 9, 5, 16);
    lmesh_route *r = lmesh_route_find(&node, 9);
    CHECK(r && r->next_hop == 2 && r->metric8 == 24 && r->seqno == 5);
    CHECK(r && r->fd_valid && r->fd_seqno == 5 && r->fd_metric8 == 24);
    CHECK(lmesh_route_next_hop(&node, 9) == 2);
    CHECK(n_events == 1 && route_event(0, LMESH_EV_ROUTE_CHANGED, 9, 2, 24, 5));
    CHECK(node.hello.trigger_pending);
    if (!r) {
        return;
    }

    lmesh_route_update(&node, n3, 9, 5, 16);
    CHECK(r->next_hop == 2 && n_events == 1);
    lmesh_route_update(&node, n3, 9, 5, 14);
    CHECK(r->next_hop == 2 && r->better_count == 0);
    lmesh_route_update(&node, n3, 9, 5, 12);
    CHECK(r->next_hop == 2 && r->better_count == 1);
    lmesh_route_update(&node, n3, 9, 5, 16);
    CHECK(r->next_hop == 2 && r->better_count == 0);
    lmesh_route_update(&node, n3, 9, 5, 12);
    CHECK(r->next_hop == 2 && r->better_count == 1);
    CHECK(n_events == 1);
    lmesh_route_update(&node, n3, 9, 5, 12);
    CHECK(r->next_hop == 3 && r->metric8 == 20 && r->better_count == 0);
    CHECK(r->fd_seqno == 5 && r->fd_metric8 == 20);
    CHECK(n_events == 2 && route_event(1, LMESH_EV_ROUTE_CHANGED, 9, 3, 20, 5));

    lmesh_route_update(&node, n3, 9, 5, LMESH_METRIC_INF);
    CHECK(r->next_hop == 2 && r->metric8 == 24);
    CHECK(r->fd_metric8 == 20);
    CHECK(n_events == 3 && route_event(2, LMESH_EV_ROUTE_CHANGED, 9, 2, 24, 5));

    lmesh_route_update(&node, n4, 9, 5, 30);
    CHECK(r->next_hop == 2 && n_events == 3);

    lmesh_route_update(&node, n2, 9, 5, LMESH_METRIC_INF);
    CHECK(r->metric8 == LMESH_METRIC_INF && r->next_hop == LMESH_ID_NONE);
    CHECK(r->retract_left == LMESH_RETRACT_REPEATS);
    CHECK(lmesh_route_next_hop(&node, 9) == LMESH_ID_NONE);
    CHECK(n_events == 4 && route_event(3, LMESH_EV_ROUTE_LOST, 9, LMESH_ID_NONE, LMESH_METRIC_INF, 5));
    CHECK(r->last_req_ms == 1000);
    uint8_t body[LMESH_ROUTE_REQ_BODY_LEN];
    lmesh_route_request_body(r, body);
    CHECK(body[0] == 9 && lmesh_get16(body + 1) == 6);

    node.now_ms = 3000;
    lmesh_route_update(&node, n4, 9, 5, 30);
    CHECK(r->metric8 == LMESH_METRIC_INF && r->last_req_ms == 1000);
    node.now_ms = 1000 + LMESH_ROUTE_REQ_GAP_MS - 1;
    lmesh_route_update(&node, n4, 9, 5, 30);
    CHECK(r->last_req_ms == 1000);
    node.now_ms = 1000 + LMESH_ROUTE_REQ_GAP_MS;
    lmesh_route_update(&node, n4, 9, 5, 30);
    CHECK(r->last_req_ms == 1000 + LMESH_ROUTE_REQ_GAP_MS);
    CHECK(n_events == 4);

    lmesh_route_update(&node, n4, 9, 6, 30);
    CHECK(r->next_hop == 4 && r->metric8 == 38 && r->seqno == 6);
    CHECK(r->fd_seqno == 6 && r->fd_metric8 == 38);
    CHECK(r->retract_left == 0);
    CHECK(n_events == 5 && route_event(4, LMESH_EV_ROUTE_CHANGED, 9, 4, 38, 6));

    lmesh_route_update(&node, n2, 1, 7, 0);
    CHECK(lmesh_route_find(&node, 1) == NULL);
    lmesh_route_update(&node, n2, 77, 7, LMESH_METRIC_INF);
    CHECK(lmesh_route_find(&node, 77) == NULL);

    n2->bidirectional = false;
    lmesh_route_update(&node, n2, 20, 1, 0);
    CHECK(lmesh_route_next_hop(&node, 20) == LMESH_ID_NONE);
    n2->bidirectional = true;
    lmesh_route_update(&node, n2, 20, 1, 250);
    CHECK(lmesh_route_find(&node, 20)->metric8 == 254);
}

static int count_retractions(const uint8_t *buf, lmesh_node_id dest)
{
    size_t r = 5u + 2u * buf[4];
    int c = 0;
    for (size_t k = 0; k < (size_t)buf[r]; k++) {
        const uint8_t *e = buf + r + 1 + 4 * k;
        c += e[0] == dest && e[3] == LMESH_METRIC_INF;
    }
    return c;
}

static void test_neighbor_down_retraction(void)
{
    setup(1);
    lmesh_neighbor *n2 = usable(2, 8);
    lmesh_neighbor *n3 = usable(3, 8);
    lmesh_route_update(&node, n2, 2, 7, 0);
    lmesh_route_update(&node, n2, 9, 3, 16);
    lmesh_route_update(&node, n3, 9, 3, 20);
    CHECK(lmesh_route_next_hop(&node, 2) == 2);
    CHECK(lmesh_route_next_hop(&node, 9) == 2);
    n_events = 0;

    for (int i = 0; i < LMESH_LINK_FAIL_LIMIT; i++) {
        lmesh_neighbor_link_result(&node, 2, false);
    }
    CHECK(!n2->up);
    CHECK(n_events == 3);
    CHECK(events[0].type == LMESH_EV_NEIGHBOR_DOWN && events[0].u.neighbor.id == 2);
    CHECK(route_event(1, LMESH_EV_ROUTE_LOST, 2, LMESH_ID_NONE, LMESH_METRIC_INF, 7));
    CHECK(route_event(2, LMESH_EV_ROUTE_CHANGED, 9, 3, 28, 3));
    for (size_t s = 0; s < LMESH_MAX_NODES; s++) {
        CHECK(!n2->adv[s].valid);
    }

    lmesh_route *r2 = lmesh_route_find(&node, 2);
    CHECK(r2 && r2->retract_left == LMESH_RETRACT_REPEATS);
    uint8_t buf[LMESH_MAX_PAYLOAD];
    for (int i = 0; i < LMESH_RETRACT_REPEATS; i++) {
        size_t len = lmesh_hello_encode(&node, buf, sizeof buf);
        CHECK(len > 0);
        CHECK(buf[4] == 1 && buf[5] == 3);
        CHECK(count_retractions(buf, 2) == 1);
        CHECK(r2 && r2->retract_left == LMESH_RETRACT_REPEATS - 1 - i);
    }
    lmesh_hello_encode(&node, buf, sizeof buf);
    CHECK(count_retractions(buf, 2) == 0);
    CHECK(r2 && r2->retract_left == 0);
}

static void test_expiry(void)
{
    setup(1);
    lmesh_neighbor *n2 = usable(2, 8);
    uint32_t t = node.now_ms;
    lmesh_route_update(&node, n2, 2, 1, 0);
    CHECK(lmesh_route_next_hop(&node, 2) == 2);
    n_events = 0;

    node.now_ms = t + LMESH_ROUTE_HOLD_MS - 1;
    node.next_wake_ms = node.now_ms + 1000000u;
    lmesh_route_tick(&node);
    CHECK(lmesh_route_next_hop(&node, 2) == 2);
    CHECK(node.next_wake_ms == t + LMESH_ROUTE_HOLD_MS);
    CHECK(n_events == 0);

    node.now_ms = t + LMESH_ROUTE_HOLD_MS;
    lmesh_route_tick(&node);
    CHECK(lmesh_route_next_hop(&node, 2) == LMESH_ID_NONE);
    CHECK(!n2->adv[0].valid);
    CHECK(n_events == 1 && route_event(0, LMESH_EV_ROUTE_LOST, 2, LMESH_ID_NONE, LMESH_METRIC_INF, 1));

    lmesh_route_update(&node, n2, 2, 1, 0);
    CHECK(lmesh_route_next_hop(&node, 2) == 2);
}

static void test_own_seqno(void)
{
    seqno_stored = true;
    lmesh_put16(seqno_slot, 100);
    lmesh_put16(seqno_slot + 2, (uint16_t)~100u);
    seqno_writes = 0;
    setup_with(1, true);
    CHECK(node.route.own_seqno == 100 + LMESH_SEQNO_BOOT_MARGIN);
    CHECK(seqno_writes == 1 && lmesh_get16(seqno_slot) == 100 + LMESH_SEQNO_BOOT_MARGIN);

    lmesh_put16(seqno_slot, 0xFFF8);
    lmesh_put16(seqno_slot + 2, (uint16_t)~0xFFF8u);
    setup_with(1, true);
    CHECK(node.route.own_seqno == (uint16_t)(0xFFF8u + LMESH_SEQNO_BOOT_MARGIN));

    lmesh_put16(seqno_slot + 2, 0);
    rnd_value = 0xBEEF;
    setup_with(1, true);
    CHECK(node.route.own_seqno == 0xBEEF);
    CHECK(lmesh_get16(seqno_slot) == 0xBEEF);

    seqno_stored = false;
    rnd_value = 0x51234;
    setup_with(1, true);
    CHECK(node.route.own_seqno == 0x1234);

    rnd_value = 0x4321;
    setup(1);
    CHECK(node.route.own_seqno == 0x4321);

    lmesh_put16(seqno_slot, 100);
    lmesh_put16(seqno_slot + 2, (uint16_t)~100u);
    seqno_stored = true;
    setup_with(1, true);
    uint16_t own = node.route.own_seqno;
    seqno_writes = 0;
    node.hello.trigger_pending = false;
    lmesh_route_rx_request(&node, 1, (uint16_t)(own - 3u));
    lmesh_route_rx_request(&node, 1, own);
    lmesh_route_rx_request(&node, 5, (uint16_t)(own + 10u));
    CHECK(node.route.own_seqno == own && seqno_writes == 0 && !node.hello.trigger_pending);
    lmesh_route_rx_request(&node, 1, (uint16_t)(own + 1u));
    CHECK(node.route.own_seqno == (uint16_t)(own + 1u));
    CHECK(seqno_writes == 1 && lmesh_get16(seqno_slot) == (uint16_t)(own + 1u));
    CHECK(node.hello.trigger_pending);

    uint8_t buf[LMESH_MAX_PAYLOAD];
    lmesh_hello_encode(&node, buf, sizeof buf);
    CHECK(lmesh_get16(buf + 1) == (uint16_t)(own + 1u));
    CHECK(buf[6] == 1 && lmesh_get16(buf + 7) == (uint16_t)(own + 1u) && buf[9] == 0);
    rnd_value = 4;
}

int main(void)
{
    test_feasibility();
    test_selection_and_requests();
    test_neighbor_down_retraction();
    test_expiry();
    test_own_seqno();
    printf("%s (%d checks)\n", failures ? "FAILED" : "ok", checks);
    return failures ? 1 : 0;
}
