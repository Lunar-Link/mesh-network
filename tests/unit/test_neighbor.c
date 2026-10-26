/* Unit tests for the neighbor table: ETX arithmetic, reception bitmap, liveness, link failures, replacement. */
#include <stdio.h>
#include <string.h>

#include "hello.h"
#include "neighbor.h"

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
#define DOWN_AFTER_MS (LMESH_HELLO_MISS_LIMIT * LMESH_HELLO_PERIOD_MS * (100 + LMESH_HELLO_JITTER_PCT) / 100)

static uint32_t clock_ms;
static uint32_t rnd_value = 4;
static lmesh_event events[MAX_EVENTS];
static int n_events;

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

static void on_event(void *ctx, const lmesh_event *ev)
{
    (void)ctx;
    if (n_events < MAX_EVENTS) {
        events[n_events] = *ev;
    }
    n_events++;
}

static lmesh_node node;

static void setup(lmesh_node_id id)
{
    lmesh_identity ident = {.id = id, .network_id = 1};
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    lmesh_callbacks cb = {.on_event = on_event};
    clock_ms = 1000;
    CHECK(lmesh_init(&node, &ident, &port, &cb) == LMESH_OK);
    n_events = 0;
}

static int count_events(lmesh_event_type type)
{
    int c = 0;
    for (int i = 0; i < n_events && i < MAX_EVENTS; i++) {
        c += events[i].type == type;
    }
    return c;
}

static void test_etx_arithmetic(void)
{
    CHECK(lmesh_etx8_compute(255, 0, false) == 16);
    CHECK(lmesh_etx8_compute(127, 0, false) == 32);
    CHECK(lmesh_etx8_compute(255, 200, false) == 16);
    CHECK(lmesh_etx8_compute(255, 255, true) == 8);
    CHECK(lmesh_etx8_compute(127, 127, true) == 32);
    CHECK(lmesh_etx8_compute(191, 255, true) == 11);
    CHECK(lmesh_etx8_compute(159, 255, true) == 13);
    CHECK(lmesh_etx8_compute(239, 255, true) == 9);
    CHECK(lmesh_etx8_compute(15, 15, true) == 254);
    CHECK(lmesh_etx8_compute(0, 255, true) == LMESH_METRIC_INF);
    CHECK(lmesh_etx8_compute(255, 0, true) == LMESH_METRIC_INF);
    CHECK(lmesh_etx8_compute(0, 0, false) == LMESH_METRIC_INF);
}

static void test_bitmap_and_bands(void)
{
    setup(1);
    lmesh_neighbor *n = lmesh_neighbor_touch(&node, 2, -80, 5);
    CHECK(n != NULL);
    CHECK(n->used && !n->up && n->etx8 == LMESH_METRIC_INF);
    CHECK(n->rssi == -80 && n->snr == 5);
    CHECK(lmesh_neighbor_find(&node, 2) == n);
    CHECK(lmesh_neighbor_etx8(n) == LMESH_METRIC_INF);

    lmesh_neighbor_hello_seen(&node, n, 10, false, 0);
    CHECK(n->up && !n->bidirectional);
    CHECK(n->rx_bitmap == 0x00FF);
    CHECK(n->rx_quality == 127);
    CHECK(n->etx8 == 32);
    CHECK(n_events == 1 && events[0].type == LMESH_EV_NEIGHBOR_UP);
    CHECK(events[0].u.neighbor.id == 2 && events[0].u.neighbor.etx8 == 32);
    CHECK(node.hello.trigger_pending);

    lmesh_neighbor_hello_seen(&node, n, 11, false, 0);
    CHECK(n->rx_bitmap == 0x01FF);
    CHECK(n->rx_quality == 143);
    CHECK(n->etx8 == 29);
    CHECK(n_events == 1);

    lmesh_neighbor_hello_seen(&node, n, 13, true, 255);
    CHECK(n->rx_bitmap == 0x07FD);
    CHECK(n->rx_quality == 159);
    CHECK(n->bidirectional && n->tx_quality == 255);
    CHECK(n->etx8 == 13);
    CHECK(n_events == 2 && events[1].type == LMESH_EV_LINK_QUALITY && events[1].u.neighbor.etx8 == 13);

    for (uint8_t s = 14; s < 30; s++) {
        lmesh_neighbor_hello_seen(&node, n, s, true, 255);
    }
    CHECK(n->rx_bitmap == 0xFFFF);
    CHECK(n->rx_quality == 255);
    CHECK(n->etx8 == 8);
    CHECK(lmesh_neighbor_etx8(n) == 8);
    CHECK(count_events(LMESH_EV_LINK_QUALITY) == 2);

    lmesh_neighbor_hello_seen(&node, n, 29, true, 255);
    CHECK(n->rx_bitmap == 0xFFFF);

    lmesh_neighbor_hello_seen(&node, n, 29 + LMESH_ETX_WINDOW + 1, true, 255);
    CHECK(n->rx_bitmap == 0x00FF);
    CHECK(n->etx8 == 16);
    CHECK(count_events(LMESH_EV_LINK_QUALITY) == 3);

    lmesh_neighbor_hello_seen(&node, n, 60, false, 0);
    CHECK(!n->bidirectional);
    CHECK(n->up);
}

static void test_missed_hellos(void)
{
    setup(1);
    lmesh_neighbor *n = lmesh_neighbor_touch(&node, 3, -70, 3);
    lmesh_neighbor_hello_seen(&node, n, 1, true, 255);
    uint32_t t = node.now_ms;

    node.now_ms = t + DOWN_AFTER_MS - 1;
    node.next_wake_ms = node.now_ms + 1000000u;
    lmesh_neighbor_tick(&node);
    CHECK(n->up);
    CHECK(node.next_wake_ms == t + DOWN_AFTER_MS);

    node.now_ms = t + DOWN_AFTER_MS;
    lmesh_neighbor_tick(&node);
    CHECK(!n->up && !n->bidirectional);
    CHECK(n->etx8 == LMESH_METRIC_INF);
    CHECK(lmesh_neighbor_etx8(n) == LMESH_METRIC_INF);
    CHECK(n_events == 2);
    CHECK(events[0].type == LMESH_EV_NEIGHBOR_UP);
    CHECK(events[1].type == LMESH_EV_NEIGHBOR_DOWN && events[1].u.neighbor.id == 3);

    node.now_ms += 100000;
    lmesh_neighbor_tick(&node);
    CHECK(n_events == 2);

    lmesh_neighbor_hello_seen(&node, n, 2, true, 255);
    CHECK(n->up);
    CHECK(n->rx_bitmap == 0x00FF);
    CHECK(n_events == 3 && events[2].type == LMESH_EV_NEIGHBOR_UP);
}

static void test_link_failures(void)
{
    setup(1);
    lmesh_neighbor *n = lmesh_neighbor_touch(&node, 4, -60, 1);
    lmesh_neighbor_hello_seen(&node, n, 1, true, 200);
    n_events = 0;

    lmesh_neighbor_link_result(&node, 4, false);
    lmesh_neighbor_link_result(&node, 4, false);
    CHECK(n->up && n->fail_count == 2);
    lmesh_neighbor_link_result(&node, 4, true);
    CHECK(n->fail_count == 0);
    for (int i = 0; i < LMESH_LINK_FAIL_LIMIT - 1; i++) {
        lmesh_neighbor_link_result(&node, 4, false);
    }
    CHECK(n->up);
    CHECK(n_events == 0);
    lmesh_neighbor_link_result(&node, 4, false);
    CHECK(!n->up);
    CHECK(n_events == 1 && events[0].type == LMESH_EV_NEIGHBOR_DOWN && events[0].u.neighbor.id == 4);

    lmesh_neighbor_link_result(&node, 4, false);
    CHECK(n_events == 1);
    lmesh_neighbor_link_result(&node, 99, false);
    CHECK(n_events == 1);

    lmesh_neighbor_hello_seen(&node, n, 2, true, 200);
    CHECK(n->up && n->fail_count == 0);
    CHECK(n_events == 2 && events[1].type == LMESH_EV_NEIGHBOR_UP);
}

static void test_table_full(void)
{
    setup(1);
    CHECK(lmesh_neighbor_touch(&node, LMESH_ID_NONE, 0, 0) == NULL);
    CHECK(lmesh_neighbor_touch(&node, LMESH_ID_ALL, 0, 0) == NULL);
    CHECK(lmesh_neighbor_touch(&node, 1, 0, 0) == NULL);

    for (int i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        node.now_ms = 1000u + (uint32_t)i;
        lmesh_neighbor *n = lmesh_neighbor_touch(&node, (lmesh_node_id)(10 + i), 0, 0);
        CHECK(n != NULL);
        if (n) {
            lmesh_neighbor_hello_seen(&node, n, 0, true, 255);
        }
    }
    node.now_ms = 2000;
    CHECK(lmesh_neighbor_touch(&node, 200, 0, 0) == NULL);

    for (int i = 0; i < LMESH_LINK_FAIL_LIMIT; i++) {
        lmesh_neighbor_link_result(&node, 15, false);
        lmesh_neighbor_link_result(&node, 12, false);
    }
    CHECK(!lmesh_neighbor_find(&node, 12)->up && !lmesh_neighbor_find(&node, 15)->up);

    lmesh_neighbor *r = lmesh_neighbor_touch(&node, 200, -50, 9);
    CHECK(r != NULL);
    CHECK(lmesh_neighbor_find(&node, 12) == NULL);
    CHECK(lmesh_neighbor_find(&node, 15) != NULL);
    CHECK(lmesh_neighbor_find(&node, 200) == r);
    CHECK(r && !r->up && r->rssi == -50 && r->last_heard_ms == 2000);
}

int main(void)
{
    test_etx_arithmetic();
    test_bitmap_and_bands();
    test_missed_hellos();
    test_link_failures();
    test_table_full();
    printf("%s (%d checks)\n", failures ? "FAILED" : "ok", checks);
    return failures ? 1 : 0;
}
