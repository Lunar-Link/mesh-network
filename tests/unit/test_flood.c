/* Unit tests for the controlled flood: header, duplicate cache, hop limit, rebroadcast delay. */
#include <stdio.h>
#include <string.h>

#include "flood.h"
#include "link.h"

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

#define MAX_CAP 16
#define SLOT (15u + 2u * LMESH_MAX_FRAME)

static uint32_t clock_ms;
static uint32_t rnd_state;
static int ncap;
static struct {
    uint8_t buf[LMESH_MAX_FRAME];
    size_t len;
} cap[MAX_CAP];

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

static int radio_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (ncap < MAX_CAP) {
        memcpy(cap[ncap].buf, frame, len);
        cap[ncap].len = len;
    }
    ncap++;
    return 0;
}

static uint32_t airtime_ms(void *ctx, size_t len)
{
    (void)ctx;
    return 15u + (uint32_t)len * 2u;
}

static lmesh_node node;
static lmesh_node peer;

static void make_node(lmesh_node *n, lmesh_node_id id)
{
    memset(n, 0, sizeof *n);
    n->ident.id = id;
    n->ident.network_id = 0x5A;
    for (unsigned i = 0; i < 16; i++) {
        n->ident.key[i] = (uint8_t)(0x30u + i);
    }
    n->port.now_ms = now_ms;
    n->port.random = rnd;
    n->port.radio_send = radio_send;
    n->port.airtime_ms = airtime_ms;
    n->link.in_flight = -1;
    n->now_ms = clock_ms;
    n->next_wake_ms = clock_ms + LMESH_TICK_MAX_SLEEP_MS;
}

static void setup(void)
{
    clock_ms = 5000;
    rnd_state = 99;
    ncap = 0;
    make_node(&node, 1);
    make_node(&peer, 2);
}

static void tick(void)
{
    node.now_ms = clock_ms;
    node.next_wake_ms = clock_ms + LMESH_TICK_MAX_SLEEP_MS;
    lmesh_link_tick(&node);
}

static int queued(void)
{
    int n = 0;
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        n += node.link.queue[i].used ? 1 : 0;
    }
    return n;
}

static const lmesh_txq_entry *first_queued(void)
{
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        if (node.link.queue[i].used) {
            return &node.link.queue[i];
        }
    }
    return NULL;
}

static bool inbound(uint8_t type, lmesh_node_id origin, uint8_t seq, uint8_t hops, const uint8_t *body, uint8_t len,
                    uint8_t total_len)
{
    uint8_t payload[LMESH_MAX_PAYLOAD];
    uint8_t buf[LMESH_MAX_FRAME];
    payload[0] = origin;
    payload[1] = seq;
    payload[2] = hops;
    memcpy(&payload[3], body, len);
    lmesh_frame_hdr h = {.type = type, .tx = peer.ident.id, .rx = LMESH_ID_ALL, .seq = 50, .payload_len = total_len};
    size_t n = lmesh_frame_encode(&peer, &h, payload, buf, sizeof buf);
    lmesh_frame_hdr d;
    const uint8_t *p;
    CHECK(lmesh_frame_decode(&node, buf, n, &d, &p) == LMESH_OK);
    return lmesh_flood_accept(&node, &d, p, d.payload_len);
}

static void test_originate(void)
{
    const uint8_t body[5] = {10, 20, 30, 40, 50};
    uint8_t big[LMESH_MAX_PAYLOAD] = {0};
    lmesh_frame_hdr h;
    const uint8_t *p;

    setup();
    CHECK(lmesh_flood_originate(&node, LMESH_FT_DATA, body, sizeof body) == LMESH_ERR_ARG);
    CHECK(lmesh_flood_originate(&node, LMESH_FT_STATUS, NULL, 3) == LMESH_ERR_ARG);
    CHECK(lmesh_flood_originate(&node, LMESH_FT_STATUS, big, LMESH_MAX_PAYLOAD - 2) == LMESH_ERR_TOO_LONG);
    CHECK(queued() == 0 && node.flood.own_seq == 0);

    CHECK(lmesh_flood_originate(&node, LMESH_FT_STATUS, body, sizeof body) == LMESH_OK);
    const lmesh_txq_entry *e = first_queued();
    CHECK(e != NULL && e->band == LMESH_PRIO_ROUTINE && e->owner == LMESH_OWNER_FLOOD && !e->want_ack);
    CHECK(e != NULL && e->due_ms == clock_ms);
    CHECK(node.flood.own_seq == 1);
    CHECK(node.flood.cache[0].used && node.flood.cache[0].origin == 1 && node.flood.cache[0].seq == 0);
    tick();
    CHECK(ncap == 1);
    CHECK(lmesh_frame_decode(&peer, cap[0].buf, cap[0].len, &h, &p) == LMESH_OK);
    CHECK(h.type == LMESH_FT_STATUS && h.tx == 1 && h.rx == LMESH_ID_ALL && h.flags == 0);
    CHECK(h.payload_len == sizeof body + LMESH_FLOOD_HEADER_LEN);
    CHECK(p[0] == 1 && p[1] == 0 && p[2] == LMESH_FLOOD_HOPS);
    CHECK(memcmp(&p[3], body, sizeof body) == 0);
    lmesh_link_tx_done(&node, true);
    CHECK(queued() == 0);

    CHECK(lmesh_flood_originate(&node, LMESH_FT_ROUTE_REQ, body, 3) == LMESH_OK);
    e = first_queued();
    CHECK(e != NULL && e->band == LMESH_PRIO_URGENT);
    CHECK(e != NULL && e->frame.buf[LMESH_HEADER_LEN + 1] == 1);
    CHECK(lmesh_flood_originate(&node, LMESH_FT_STATUS, big, LMESH_MAX_PAYLOAD - LMESH_FLOOD_HEADER_LEN) ==
          LMESH_OK);
    CHECK(node.flood.own_seq == 3);

    CHECK(!inbound(LMESH_FT_STATUS, 1, 0, 3, body, 2, 5));
    CHECK(!inbound(LMESH_FT_STATUS, 1, 77, 3, body, 2, 5));
}

static void test_accept_and_rebroadcast(void)
{
    const uint8_t body[4] = {1, 2, 3, 4};
    const uint32_t slot = SLOT;
    lmesh_frame_hdr h;
    const uint8_t *p;

    setup();
    CHECK(inbound(LMESH_FT_STATUS, 9, 4, 4, body, 4, 7));
    CHECK(queued() == 1);
    const lmesh_txq_entry *e = first_queued();
    uint32_t delay = e->due_ms - clock_ms;
    CHECK(delay % slot == 0 && delay >= slot && delay <= 3 * slot);
    CHECK(e->band == LMESH_PRIO_ROUTINE && e->owner == LMESH_OWNER_FLOOD && !e->want_ack);

    CHECK(!inbound(LMESH_FT_STATUS, 9, 4, 4, body, 4, 7));
    CHECK(!inbound(LMESH_FT_STATUS, 9, 4, 2, body, 4, 7));
    CHECK(queued() == 1);

    uint32_t due = e->due_ms;
    clock_ms = due - 1;
    tick();
    CHECK(ncap == 0);
    clock_ms = due;
    tick();
    CHECK(ncap == 1);
    CHECK(lmesh_frame_decode(&peer, cap[0].buf, cap[0].len, &h, &p) == LMESH_OK);
    CHECK(h.type == LMESH_FT_STATUS && h.tx == 1 && h.rx == LMESH_ID_ALL && h.payload_len == 7);
    CHECK(p[0] == 9 && p[1] == 4 && p[2] == 3 && memcmp(&p[3], body, 4) == 0);
    lmesh_link_tx_done(&node, true);

    CHECK(inbound(LMESH_FT_ROUTE_REQ, 9, 5, 2, body, 3, 6));
    e = first_queued();
    CHECK(e != NULL && e->band == LMESH_PRIO_URGENT && e->frame.buf[LMESH_HEADER_LEN + 2] == 1);
    CHECK(e != NULL && (int)e->frame.buf[0] == ((LMESH_FRAME_VERSION << 4) | LMESH_FT_ROUTE_REQ));

    setup();
    CHECK(inbound(LMESH_FT_STATUS, 9, 1, 1, body, 4, 7));
    CHECK(inbound(LMESH_FT_STATUS, 9, 2, 0, body, 4, 7));
    CHECK(queued() == 0);
    CHECK(!inbound(LMESH_FT_STATUS, 9, 3, 4, body, 0, 2));
    CHECK(!inbound(LMESH_FT_STATUS, LMESH_ID_NONE, 3, 4, body, 0, 3));
    CHECK(inbound(LMESH_FT_STATUS, 9, 3, 4, body, 0, 3));
    CHECK(queued() == 1);

    uint32_t seen_min = 99, seen_max = 0;
    for (unsigned trial = 0; trial < 60; trial++) {
        setup();
        rnd_state = trial * 13u + 1u;
        CHECK(inbound(LMESH_FT_STATUS, 9, 4, 4, body, 4, 7));
        uint32_t k = (first_queued()->due_ms - clock_ms) / slot;
        seen_min = k < seen_min ? k : seen_min;
        seen_max = k > seen_max ? k : seen_max;
    }
    CHECK(seen_min == 1 && seen_max == 3);

    setup();
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        CHECK(inbound(LMESH_FT_STATUS, 9, (uint8_t)i, 4, body, 4, 7));
    }
    CHECK(queued() == LMESH_TXQ_SIZE);
    CHECK(inbound(LMESH_FT_STATUS, 9, 200, 4, body, 4, 7));
    CHECK(!inbound(LMESH_FT_STATUS, 9, 200, 4, body, 4, 7));
}

static void test_ring(void)
{
    const uint8_t body[1] = {0};
    setup();
    for (unsigned s = 0; s < LMESH_FLOOD_CACHE; s++) {
        CHECK(inbound(LMESH_FT_STATUS, 5, (uint8_t)s, 1, body, 1, 4));
    }
    for (unsigned s = 0; s < LMESH_FLOOD_CACHE; s++) {
        CHECK(!inbound(LMESH_FT_STATUS, 5, (uint8_t)s, 1, body, 1, 4));
    }
    CHECK(inbound(LMESH_FT_STATUS, 6, 0, 1, body, 1, 4));
    CHECK(inbound(LMESH_FT_STATUS, 5, 0, 1, body, 1, 4));
    CHECK(!inbound(LMESH_FT_STATUS, 5, 2, 1, body, 1, 4));
    CHECK(inbound(LMESH_FT_STATUS, 5, 1, 1, body, 1, 4));
    CHECK(!inbound(LMESH_FT_STATUS, 6, 0, 1, body, 1, 4));
    CHECK(queued() == 0);
}

int main(void)
{
    test_originate();
    test_accept_and_rebroadcast();
    test_ring();

    printf("%d checks, %d failures\n", checks, failures);
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
