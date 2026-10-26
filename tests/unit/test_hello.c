/* Unit tests for HELLO: encoding, decode round trip, pagination, malformed input, timing, restart and vector replacement. */
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

static uint32_t clock_ms;
static uint32_t rnd_value = 4;
static int n_events;
static int ev_count[LMESH_EV_STORE + 1];

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
    n_events++;
    if (ev->type <= LMESH_EV_STORE) {
        ev_count[ev->type]++;
    }
}

static lmesh_node node_a;
static lmesh_node node_b;
static lmesh_node node_c;

static void setup(lmesh_node *node, lmesh_node_id id, uint32_t random)
{
    lmesh_identity ident = {.id = id, .network_id = 1};
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    lmesh_callbacks cb = {.on_event = on_event};
    clock_ms = 1000;
    rnd_value = random;
    CHECK(lmesh_init(node, &ident, &port, &cb) == LMESH_OK);
}

static lmesh_frame_hdr hello_hdr(lmesh_node_id tx, size_t len)
{
    lmesh_frame_hdr h = {.version = LMESH_FRAME_VERSION, .type = LMESH_FT_HELLO, .network_id = 1, .tx = tx,
                         .rx = LMESH_ID_ALL, .payload_len = (uint8_t)len};
    return h;
}

static void deliver(lmesh_node *to, lmesh_node_id from, const uint8_t *buf, size_t len)
{
    lmesh_frame_hdr h = hello_hdr(from, len);
    lmesh_hello_rx(to, &h, buf, len, -90, 4);
}

static size_t route_offset(const uint8_t *buf)
{
    return 5u + 2u * buf[4];
}

static void test_encode_alone(void)
{
    uint8_t buf[LMESH_MAX_PAYLOAD];
    setup(&node_a, 1, 0x1111);
    CHECK(node_a.route.own_seqno == 0x1111);
    size_t len = lmesh_hello_encode(&node_a, buf, sizeof buf);
    CHECK(len == 10);
    CHECK(buf[0] == 0);
    CHECK(lmesh_get16(buf + 1) == 0x1111);
    CHECK(buf[3] == 0);
    CHECK(buf[4] == 0);
    CHECK(buf[5] == 1);
    CHECK(buf[6] == 1 && lmesh_get16(buf + 7) == 0x1111 && buf[9] == 0);
    len = lmesh_hello_encode(&node_a, buf, sizeof buf);
    CHECK(buf[0] == 1);
    CHECK(lmesh_hello_encode(&node_a, buf, 9) == 0);
    CHECK(lmesh_hello_encode(&node_a, NULL, 100) == 0);
}

static void test_round_trip(void)
{
    uint8_t buf[LMESH_MAX_PAYLOAD];
    size_t len;

    setup(&node_a, 1, 0x1111);
    setup(&node_b, 2, 0x2222);

    len = lmesh_hello_encode(&node_a, buf, sizeof buf);
    deliver(&node_b, 1, buf, len);
    lmesh_neighbor *b1 = lmesh_neighbor_find(&node_b, 1);
    CHECK(b1 != NULL);
    CHECK(b1 && b1->up && !b1->bidirectional && b1->rx_quality == 127);
    CHECK(lmesh_route_next_hop(&node_b, 1) == LMESH_ID_NONE);
    CHECK(node_b.stats.rx_malformed == 0);

    len = lmesh_hello_encode(&node_b, buf, sizeof buf);
    CHECK(buf[4] == 1 && buf[5] == 1 && buf[6] == 127);
    CHECK(buf[7] == 1);
    CHECK(len == 12);
    deliver(&node_a, 2, buf, len);
    lmesh_neighbor *a2 = lmesh_neighbor_find(&node_a, 2);
    CHECK(a2 && a2->up && a2->bidirectional && a2->tx_quality == 127);
    CHECK(a2 && a2->etx8 == 32);
    CHECK(lmesh_route_next_hop(&node_a, 2) == 2);
    lmesh_route *ra = lmesh_route_find(&node_a, 2);
    CHECK(ra && ra->metric8 == 32 && ra->seqno == 0x2222);

    len = lmesh_hello_encode(&node_a, buf, sizeof buf);
    CHECK(len == 16);
    CHECK(buf[4] == 1 && buf[5] == 2 && buf[6] == 127);
    size_t r = route_offset(buf);
    CHECK(buf[r] == 2);
    CHECK(buf[r + 1] == 1 && lmesh_get16(buf + r + 2) == 0x1111 && buf[r + 4] == 0);
    CHECK(buf[r + 5] == 2 && lmesh_get16(buf + r + 6) == 0x2222 && buf[r + 8] == 32);

    deliver(&node_b, 1, buf, len);
    CHECK(b1 && b1->bidirectional && b1->rx_bitmap == 0x01FF && b1->etx8 == 29);
    CHECK(lmesh_route_next_hop(&node_b, 1) == 1);
    lmesh_route *rb = lmesh_route_find(&node_b, 1);
    CHECK(rb && rb->metric8 == 29 && rb->seqno == 0x1111);
    CHECK(lmesh_route_find(&node_b, 2) == NULL);
    CHECK(node_a.stats.rx_malformed == 0 && node_b.stats.rx_malformed == 0);
}

static void parse_dests(const uint8_t *buf, size_t len, bool seen[256], int *count)
{
    size_t r = route_offset(buf);
    CHECK(len == r + 1u + 4u * buf[r]);
    for (size_t k = 0; k < (size_t)buf[r]; k++) {
        lmesh_node_id d = buf[r + 1 + 4 * k];
        CHECK(!seen[d]);
        seen[d] = true;
        (*count)++;
    }
}

static void test_pagination(void)
{
    uint8_t buf[LMESH_MAX_PAYLOAD];
    size_t len;

    setup(&node_c, 50, 0x0500);
    for (int i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        lmesh_node_id id = (lmesh_node_id)(2 + i);
        lmesh_neighbor *n = lmesh_neighbor_touch(&node_c, id, -80, 0);
        CHECK(n != NULL);
        if (n) {
            lmesh_neighbor_hello_seen(&node_c, n, 0, true, 255);
            lmesh_route_update(&node_c, n, id, (uint16_t)(100 + id), 0);
        }
    }
    for (int i = 0; i < LMESH_MAX_NODES; i++) {
        CHECK(node_c.route.table[i].used && node_c.route.table[i].metric8 != LMESH_METRIC_INF);
    }

    len = lmesh_hello_encode(&node_c, buf, sizeof buf);
    CHECK(len == 6u + 2u * LMESH_MAX_NEIGHBORS + 4u * (LMESH_MAX_NODES + 1u));
    CHECK(len <= LMESH_MAX_PAYLOAD);
    CHECK((buf[3] & LMESH_HELLO_MORE) == 0);
    CHECK(buf[4] == LMESH_MAX_NEIGHBORS);
    bool seen[256] = {false};
    int count = 0;
    parse_dests(buf, len, seen, &count);
    CHECK(count == LMESH_MAX_NODES + 1);
    CHECK(seen[50]);

    setup(&node_b, 60, 0x0600);
    const size_t small = 60;
    const size_t per_page = (small - 6u - 2u * LMESH_MAX_NEIGHBORS) / 4u;
    const size_t total = LMESH_MAX_NODES + 1u;
    const size_t pages = (total + per_page - 1u) / per_page;
    memset(seen, 0, sizeof seen);
    count = 0;
    for (size_t p = 0; p < pages; p++) {
        len = lmesh_hello_encode(&node_c, buf, small);
        CHECK(len > 0 && len <= small);
        bool more = (buf[3] & LMESH_HELLO_MORE) != 0;
        CHECK(more == (p + 1u < pages));
        parse_dests(buf, len, seen, &count);
        deliver(&node_b, 50, buf, len);
    }
    CHECK(count == (int)total);
    CHECK(seen[50]);
    for (int id = 2; id < 2 + LMESH_MAX_NEIGHBORS; id++) {
        CHECK(seen[id]);
    }
    CHECK(node_b.stats.rx_malformed == 0);
    CHECK(lmesh_neighbor_find(&node_b, 50) != NULL);

    len = lmesh_hello_encode(&node_c, buf, small);
    CHECK(buf[route_offset(buf) + 1] == 50);
    CHECK((size_t)buf[route_offset(buf)] == per_page);
}

static void test_malformed(void)
{
    setup(&node_a, 70, 1);
    static const uint8_t too_short[] = {0, 0, 0, 0, 0};
    static const uint8_t short_neighbors[] = {0, 0, 0, 0, 2, 0};
    static const uint8_t short_routes[] = {0, 0, 0, 0, 0, 1};
    static const uint8_t trailing[] = {0, 0, 0, 0, 0, 0, 9};
    static const uint8_t minimal[] = {0, 0, 0, 0, 0, 0};

    deliver(&node_a, 3, too_short, sizeof too_short);
    deliver(&node_a, 3, short_neighbors, sizeof short_neighbors);
    deliver(&node_a, 3, short_routes, sizeof short_routes);
    deliver(&node_a, 3, trailing, sizeof trailing);
    deliver(&node_a, LMESH_ID_NONE, minimal, sizeof minimal);
    deliver(&node_a, 3, NULL, 0);
    CHECK(node_a.stats.rx_malformed == 6);
    CHECK(lmesh_neighbor_find(&node_a, 3) == NULL);

    deliver(&node_a, 3, minimal, sizeof minimal);
    CHECK(node_a.stats.rx_malformed == 6);
    CHECK(lmesh_neighbor_find(&node_a, 3) != NULL);

    deliver(&node_a, 70, minimal, sizeof minimal);
    CHECK(lmesh_neighbor_find(&node_a, 70) == NULL);
}

static void test_timing(void)
{
    setup(&node_a, 80, 2500);
    lmesh_hello_state *h = &node_a.hello;
    CHECK(h->next_ms == 3500);
    CHECK(!h->trigger_pending);

    node_a.now_ms = 3499;
    node_a.next_wake_ms = node_a.now_ms + 1000000u;
    lmesh_hello_tick(&node_a);
    CHECK(node_a.nbr.hello_seq == 0);
    CHECK(node_a.next_wake_ms == 3500);

    node_a.now_ms = 3500;
    lmesh_hello_tick(&node_a);
    CHECK(node_a.nbr.hello_seq == 1);
    CHECK(h->next_ms == 3500u + 8000u + 2500u);

    node_a.now_ms = 4000;
    lmesh_hello_trigger(&node_a);
    CHECK(h->trigger_pending && h->trigger_at_ms == 4000u + 2500u % 1001u);
    lmesh_hello_trigger(&node_a);
    CHECK(h->trigger_at_ms == 4498);

    node_a.now_ms = 4497;
    node_a.next_wake_ms = node_a.now_ms + 1000000u;
    lmesh_hello_tick(&node_a);
    CHECK(node_a.nbr.hello_seq == 1);
    CHECK(node_a.next_wake_ms == 4498);

    node_a.now_ms = 4498;
    lmesh_hello_tick(&node_a);
    CHECK(node_a.nbr.hello_seq == 2);
    CHECK(!h->trigger_pending && h->last_triggered_ms == 4498);
    CHECK(h->next_ms == 14000);

    node_a.now_ms = 4600;
    lmesh_hello_trigger(&node_a);
    CHECK(h->trigger_at_ms == 4498u + LMESH_TRIGGER_MIN_GAP_MS);

    node_a.now_ms = 14000;
    lmesh_hello_tick(&node_a);
    CHECK(node_a.nbr.hello_seq == 3);
    CHECK(!h->trigger_pending);
}

typedef struct adv_entry {
    lmesh_node_id dest;
    uint16_t seqno;
    uint8_t metric8;
} adv_entry;

static size_t hello_pkt(uint8_t *buf, uint8_t seq, uint16_t own, uint8_t flags, lmesh_node_id listed,
                        const adv_entry *r, size_t m)
{
    size_t pos = 0;
    buf[pos++] = seq;
    lmesh_put16(buf + pos, own);
    pos += 2;
    buf[pos++] = flags;
    buf[pos++] = 1;
    buf[pos++] = listed;
    buf[pos++] = 255;
    buf[pos++] = (uint8_t)m;
    for (size_t k = 0; k < m; k++, pos += 4) {
        buf[pos] = r[k].dest;
        lmesh_put16(buf + pos + 1, r[k].seqno);
        buf[pos + 3] = r[k].metric8;
    }
    return pos;
}

static void send_from(lmesh_node *to, lmesh_node_id from, uint8_t seq, uint16_t own, uint8_t flags,
                      const adv_entry *r, size_t m)
{
    uint8_t buf[LMESH_MAX_PAYLOAD];
    to->now_ms += 1000;
    deliver(to, from, buf, hello_pkt(buf, seq, own, flags, to->ident.id, r, m));
}

static void clear_events(void)
{
    memset(ev_count, 0, sizeof ev_count);
}

static const lmesh_adv *adv_for(lmesh_node *node, const lmesh_neighbor *n, lmesh_node_id dest)
{
    lmesh_route *r = lmesh_route_find(node, dest);
    return r && n ? &n->adv[r - node->route.table] : NULL;
}

static void setup_vector(uint8_t seq)
{
    static const adv_entry full[] = {{20, 500, 0}, {30, 300, 16}, {31, 310, 24}};
    setup(&node_a, 10, 0x1000);
    send_from(&node_a, 20, seq, 500, 0, full, 3);
    CHECK(lmesh_route_next_hop(&node_a, 30) == 20 && lmesh_route_next_hop(&node_a, 31) == 20);
    clear_events();
}

static void test_restart(void)
{
    setup_vector(50);
    lmesh_neighbor *n = lmesh_neighbor_find(&node_a, 20);
    static const adv_entry page0[] = {{20, 516, 0}};
    send_from(&node_a, 20, 0, 516, LMESH_HELLO_MORE, page0, 1);
    CHECK(ev_count[LMESH_EV_NEIGHBOR_DOWN] == 1 && ev_count[LMESH_EV_NEIGHBOR_UP] == 1);
    CHECK(ev_count[LMESH_EV_ROUTE_LOST] >= 2);
    CHECK(lmesh_route_next_hop(&node_a, 30) == LMESH_ID_NONE && lmesh_route_next_hop(&node_a, 31) == LMESH_ID_NONE);
    CHECK(!adv_for(&node_a, n, 30)->valid && !adv_for(&node_a, n, 31)->valid);
    CHECK(n && n->up && n->bidirectional && n->rx_bitmap == 0x00FF && n->own_seqno == 516);
    lmesh_route *r = lmesh_route_find(&node_a, 20);
    CHECK(r && r->next_hop == 20 && r->seqno == 516);

    setup_vector(40);
    clear_events();
    send_from(&node_a, 20, 80, 501, LMESH_HELLO_MORE, page0, 1);
    CHECK(ev_count[LMESH_EV_NEIGHBOR_DOWN] == 1 && lmesh_route_next_hop(&node_a, 30) == LMESH_ID_NONE);
}

static void test_seqno_bump(void)
{
    setup_vector(50);
    lmesh_neighbor *n = lmesh_neighbor_find(&node_a, 20);
    static const adv_entry bumped[] = {{20, 501, 0}, {30, 300, 16}, {31, 310, 24}};
    send_from(&node_a, 20, 51, 501, 0, bumped, 3);
    CHECK(ev_count[LMESH_EV_NEIGHBOR_DOWN] == 0 && ev_count[LMESH_EV_NEIGHBOR_UP] == 0);
    CHECK(ev_count[LMESH_EV_ROUTE_LOST] == 0);
    CHECK(lmesh_route_next_hop(&node_a, 30) == 20 && lmesh_route_next_hop(&node_a, 31) == 20);
    CHECK(n && n->rx_bitmap == 0x01FF && n->own_seqno == 501);
    lmesh_route *r = lmesh_route_find(&node_a, 20);
    CHECK(r && r->seqno == 501);

    static const adv_entry page0[] = {{20, 502, 0}, {30, 300, 16}};
    send_from(&node_a, 20, 52, 502, LMESH_HELLO_MORE, page0, 2);
    CHECK(ev_count[LMESH_EV_NEIGHBOR_DOWN] == 0);
    CHECK(lmesh_route_next_hop(&node_a, 30) == 20 && lmesh_route_next_hop(&node_a, 31) == LMESH_ID_NONE);
}

static void test_complete_hello(void)
{
    setup_vector(50);
    lmesh_neighbor *n = lmesh_neighbor_find(&node_a, 20);
    static const adv_entry partial[] = {{20, 500, 0}, {30, 300, 16}};
    send_from(&node_a, 20, 51, 500, 0, partial, 2);
    CHECK(ev_count[LMESH_EV_ROUTE_LOST] == 1 && ev_count[LMESH_EV_NEIGHBOR_DOWN] == 0);
    CHECK(lmesh_route_next_hop(&node_a, 30) == 20 && lmesh_route_next_hop(&node_a, 31) == LMESH_ID_NONE);
    CHECK(!adv_for(&node_a, n, 31)->valid);

    static const adv_entry with31[] = {{20, 500, 0}, {30, 300, 16}, {31, 310, 24}};
    send_from(&node_a, 20, 52, 500, 0, with31, 3);
    CHECK(lmesh_route_next_hop(&node_a, 31) == 20);
    static const adv_entry first_page[] = {{20, 500, 0}};
    static const adv_entry last_page[] = {{31, 310, 24}};
    send_from(&node_a, 20, 53, 500, LMESH_HELLO_MORE, first_page, 1);
    send_from(&node_a, 20, 54, 500, 0, last_page, 1);
    CHECK(lmesh_route_next_hop(&node_a, 30) == 20 && lmesh_route_next_hop(&node_a, 31) == 20);
}

static void test_wraparound(void)
{
    static const adv_entry full[] = {{20, 500, 0}, {30, 300, 16}, {31, 310, 24}};
    setup_vector(254);
    lmesh_neighbor *n = lmesh_neighbor_find(&node_a, 20);
    send_from(&node_a, 20, 255, 500, 0, full, 3);
    send_from(&node_a, 20, 0, 500, 0, full, 3);
    send_from(&node_a, 20, 1, 500, 0, full, 3);
    CHECK(ev_count[LMESH_EV_NEIGHBOR_DOWN] == 0 && ev_count[LMESH_EV_ROUTE_LOST] == 0);
    CHECK(n && n->rx_bitmap == 0x07FF);
    CHECK(lmesh_route_next_hop(&node_a, 30) == 20);

    send_from(&node_a, 20, 0, 500, 0, full, 3);
    CHECK(ev_count[LMESH_EV_NEIGHBOR_DOWN] == 0);
    send_from(&node_a, 20, 253, 500, 0, full, 3);
    CHECK(ev_count[LMESH_EV_NEIGHBOR_DOWN] == 1 && ev_count[LMESH_EV_NEIGHBOR_UP] == 1);
}

static void test_stale_echo(void)
{
    setup_vector(50);
    static const adv_entry stale[] = {{20, 500, 0}, {10, 0x0FF0, 16}, {30, 300, 40}, {31, 310, LMESH_METRIC_INF}};
    lmesh_route *r30 = lmesh_route_find(&node_a, 30);
    uint8_t metric30 = r30 ? r30->metric8 : 0;
    send_from(&node_a, 20, 51, 500, 0, stale, 4);
    CHECK(r30 && r30->next_hop == 20 && r30->metric8 == metric30);
    CHECK(lmesh_route_next_hop(&node_a, 31) == LMESH_ID_NONE);

    setup(&node_b, 11, 0x2000);
    static const adv_entry before_boot[] = {{20, 500, 0}, {11, 0x1F00, 8}, {30, 300, 16}};
    send_from(&node_b, 20, 7, 500, 0, before_boot, 3);
    CHECK(lmesh_route_next_hop(&node_b, 20) == 20 && lmesh_route_next_hop(&node_b, 30) == LMESH_ID_NONE);
    static const adv_entry after_boot[] = {{20, 500, 0}, {11, 0x2000, 8}, {30, 300, 16}};
    send_from(&node_b, 20, 8, 500, 0, after_boot, 3);
    CHECK(lmesh_route_next_hop(&node_b, 30) == 20);
}

int main(void)
{
    test_encode_alone();
    test_round_trip();
    test_pagination();
    test_malformed();
    test_timing();
    test_restart();
    test_seqno_bump();
    test_complete_hello();
    test_wraparound();
    test_stale_echo();
    printf("%s (%d checks)\n", failures ? "FAILED" : "ok", checks);
    return failures ? 1 : 0;
}
