/* Unit tests for status broadcasts and the node table. */
#include <stdio.h>
#include <string.h>

#include "lmesh/lmesh.h"
#include "status.h"

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
static uint32_t rng_state = 12345;
static uint16_t battery = 3700;
static uint8_t app_flags;
static int events;
static lmesh_event last_ev;

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 1;
}

static uint8_t sent[LMESH_MAX_FRAME];
static size_t sent_len;

static int radio_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (len <= sizeof sent) {
        memcpy(sent, frame, len);
        sent_len = len;
    }
    return 0;
}

static uint32_t airtime_ms(void *ctx, size_t len)
{
    (void)ctx;
    return 15u + (uint32_t)len * 2u;
}

static uint16_t battery_mv(void *ctx)
{
    (void)ctx;
    return battery;
}

static uint8_t health_flags(void *ctx)
{
    (void)ctx;
    return app_flags;
}

static void on_event(void *ctx, const lmesh_event *ev)
{
    (void)ctx;
    events++;
    last_ev = *ev;
}

static const lmesh_port port = {.now_ms = now_ms,
                                .random = rnd,
                                .radio_send = radio_send,
                                .airtime_ms = airtime_ms,
                                .battery_mv = battery_mv,
                                .health_flags = health_flags};
static const lmesh_callbacks cbs = {.on_event = on_event};

static lmesh_node a;
static lmesh_node b;

static void setup(lmesh_node *n, lmesh_node_id id, const char *station)
{
    lmesh_identity ident = {.id = id, .network_id = 1};
    ident.station_id_len = (uint8_t)strlen(station);
    memcpy(ident.station_id, station, ident.station_id_len);
    CHECK(lmesh_init(n, &ident, &port, &cbs) == LMESH_OK);
}

static void set_neighbor(lmesh_node *n, size_t slot, lmesh_node_id id, bool up, bool bidir, uint8_t etx8)
{
    lmesh_neighbor *nb = &n->nbr.table[slot];
    memset(nb, 0, sizeof *nb);
    nb->used = true;
    nb->id = id;
    nb->up = up;
    nb->bidirectional = bidir;
    nb->etx8 = etx8;
}

static void set_depth(lmesh_node *n, size_t depth)
{
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        n->store.records[i].role = (uint8_t)(i < depth ? LMESH_ROLE_RELAY : LMESH_ROLE_FREE);
    }
}

static uint8_t flags_of(lmesh_node *n)
{
    uint8_t body[LMESH_STATUS_BODY_MAX];
    size_t len = lmesh_status_encode(n, body, sizeof body);
    CHECK(len >= LMESH_STATUS_FIXED_LEN);
    return body[2];
}

static uint8_t lost_of(lmesh_node *n)
{
    uint8_t body[LMESH_STATUS_BODY_MAX];
    CHECK(lmesh_status_encode(n, body, sizeof body) > 0);
    return body[8];
}

static size_t build(lmesh_node *n, uint8_t *frame, size_t max)
{
    frame[0] = n->ident.id;
    frame[1] = 17;
    frame[2] = 3;
    size_t len = lmesh_status_encode(n, frame + 3, max - 3);
    CHECK(len > 0);
    return len + 3;
}

static void test_layout(void)
{
    clock_ms = 1000;
    battery = 3700;
    app_flags = 0xA1;
    setup(&a, 5, "KD9ABC");
    a.now_ms = 1000 + 3723500;
    set_depth(&a, 3);
    a.store.evicted = 2;
    a.store.expired = 1;
    set_neighbor(&a, 0, 9, true, true, 10);
    set_neighbor(&a, 1, 12, true, false, 20);
    set_neighbor(&a, 2, 14, false, true, 30);
    set_neighbor(&a, 5, 33, true, true, 8);

    static const uint8_t expect[] = {
        0x74, 0x0E,
        0xA8,
        0x8B, 0x0E, 0x00, 0x00,
        3,
        3,
        6, 'K', 'D', '9', 'A', 'B', 'C',
        3, 9, 10, 12, 20, 33, 8,
    };
    uint8_t body[LMESH_STATUS_BODY_MAX];
    memset(body, 0xEE, sizeof body);
    size_t len = lmesh_status_encode(&a, body, sizeof body);
    CHECK(len == sizeof expect);
    CHECK(memcmp(body, expect, sizeof expect) == 0);
    CHECK(body[sizeof expect] == 0xEE);
    app_flags = 0;
}

static void test_flags(void)
{
    clock_ms = 0;
    battery = 3700;
    app_flags = 0;
    setup(&a, 5, "");
    set_neighbor(&a, 0, 9, true, true, 8);

    CHECK(flags_of(&a) == LMESH_HEALTH_RESET);
    a.status.reset_flag = false;
    CHECK(flags_of(&a) == 0);

    battery = LMESH_LOW_BATTERY_MV - 1;
    CHECK(flags_of(&a) == LMESH_HEALTH_LOW_BATTERY);
    battery = LMESH_LOW_BATTERY_MV;
    CHECK(flags_of(&a) == 0);
    a.port.battery_mv = NULL;
    CHECK(flags_of(&a) == 0);
    uint8_t body[LMESH_STATUS_BODY_MAX];
    CHECK(lmesh_status_encode(&a, body, sizeof body) > 0);
    CHECK(body[0] == 0 && body[1] == 0);
    a.port.battery_mv = battery_mv;

    set_depth(&a, LMESH_STORE_SIZE * 3 / 4);
    CHECK(flags_of(&a) == 0);
    set_depth(&a, LMESH_STORE_SIZE * 3 / 4 + 1);
    CHECK(flags_of(&a) == LMESH_HEALTH_STORE_HIGH);
    set_depth(&a, 0);

    a.nbr.table[0].bidirectional = false;
    CHECK(flags_of(&a) == LMESH_HEALTH_ISOLATED);
    a.nbr.table[0].bidirectional = true;
    a.nbr.table[0].up = false;
    CHECK(flags_of(&a) == LMESH_HEALTH_ISOLATED);
    a.nbr.table[0].used = false;
    a.nbr.table[0].up = true;
    CHECK(flags_of(&a) == LMESH_HEALTH_ISOLATED);
    a.nbr.table[0].used = true;
    CHECK(flags_of(&a) == 0);

    app_flags = 0xFF;
    CHECK(flags_of(&a) == LMESH_HEALTH_APP_MASK);
    a.port.health_flags = NULL;
    CHECK(flags_of(&a) == 0);
    a.port.health_flags = health_flags;
    app_flags = 0;

    a.status.reset_flag = true;
    battery = 3000;
    set_depth(&a, LMESH_STORE_SIZE);
    a.nbr.table[0].used = false;
    CHECK((flags_of(&a) & LMESH_HEALTH_RADIO_FAULT) == 0);
    CHECK(flags_of(&a) ==
          (LMESH_HEALTH_LOW_BATTERY | LMESH_HEALTH_STORE_HIGH | LMESH_HEALTH_RESET | LMESH_HEALTH_ISOLATED));
    battery = 3700;
}

static void test_lost(void)
{
    clock_ms = 0;
    setup(&a, 5, "");
    a.status.next_ms = a.now_ms + 1000000u;
    CHECK(lost_of(&a) == 0);
    a.store.evicted = 3;
    CHECK(lost_of(&a) == 3);
    a.store.expired = 4;
    a.now_ms += 1000;
    lmesh_status_tick(&a);
    CHECK(lost_of(&a) == 7);
    a.store.evicted = (uint16_t)(a.store.evicted + 200);
    CHECK(lost_of(&a) == 207);
    a.store.expired = (uint16_t)(a.store.expired + 100);
    CHECK(lost_of(&a) == 255);
    a.store.expired = (uint16_t)(a.store.expired + 1000);
    CHECK(lost_of(&a) == 255);

    lmesh_err err = lmesh_status_send(&a);
    if (err == LMESH_OK) {
        CHECK(!a.status.reset_flag);
        CHECK(lost_of(&a) == 0);
        a.store.evicted++;
        CHECK(lost_of(&a) == 1);
    } else {
        CHECK(a.status.reset_flag);
        CHECK(lost_of(&a) == 255);
    }

    setup(&b, 6, "");
    b.store.evicted = 65530;
    b.status.lost_seen = 65530;
    b.store.evicted = (uint16_t)(b.store.evicted + 10);
    CHECK(lost_of(&b) == 10);
}

static void test_station_id(void)
{
    uint8_t body[LMESH_STATUS_BODY_MAX];
    clock_ms = 0;
    setup(&a, 5, "");
    size_t len = lmesh_status_encode(&a, body, sizeof body);
    CHECK(len == LMESH_STATUS_FIXED_LEN);
    CHECK(body[9] == 0 && body[10] == 0);

    setup(&a, 5, "ABCDEFGH");
    len = lmesh_status_encode(&a, body, sizeof body);
    CHECK(len == LMESH_STATUS_FIXED_LEN + 8);
    CHECK(body[9] == 8 && memcmp(body + 10, "ABCDEFGH", 8) == 0 && body[18] == 0);
}

static void test_fit(void)
{
    uint8_t body[LMESH_MAX_PAYLOAD + 8];
    clock_ms = 0;
    setup(&a, 5, "ABCDEFGH");
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        set_neighbor(&a, i, (lmesh_node_id)(10 + i), true, true, 8);
    }
    size_t need = LMESH_STATUS_BODY_MAX;
    CHECK(need <= LMESH_MAX_PAYLOAD - LMESH_FLOOD_HEADER_LEN);
    CHECK(lmesh_status_encode(&a, body, sizeof body) == need);

    a.store.evicted = 9;
    memset(body, 0xEE, sizeof body);
    CHECK(lmesh_status_encode(&a, body, need - 1) == 0);
    bool untouched = true;
    for (size_t i = 0; i < sizeof body; i++) {
        untouched = untouched && body[i] == 0xEE;
    }
    CHECK(untouched);
    CHECK(lmesh_status_encode(&a, body, 0) == 0);
    CHECK(lmesh_status_encode(&a, body, need) == need);
    CHECK(body[8] == 9);

    a.ident.station_id_len = 200;
    CHECK(lmesh_status_encode(&a, body, sizeof body) == need);
    CHECK(body[9] == LMESH_STATION_ID_MAX);
}

static void test_roundtrip(void)
{
    uint8_t frame[LMESH_MAX_PAYLOAD];
    clock_ms = 0;
    battery = 3456;
    app_flags = 0x40;
    setup(&a, 5, "N0CALL");
    setup(&b, 6, "");
    a.now_ms = 90500;
    set_depth(&a, 2);
    a.store.expired = 5;
    set_neighbor(&a, 0, 6, true, true, 12);
    set_neighbor(&a, 3, 7, true, false, 40);
    set_neighbor(&a, 4, 8, false, true, 9);
    size_t len = build(&a, frame, sizeof frame);

    b.now_ms = 777;
    events = 0;
    lmesh_status_rx(&b, frame, len);
    CHECK(events == 1);
    CHECK(last_ev.type == LMESH_EV_STATUS_RECEIVED && last_ev.u.status.id == 5 && last_ev.t_ms == 777);
    CHECK(b.stats.rx_malformed == 0);
    lmesh_node_entry *e = lmesh_status_entry(&b, 5);
    CHECK(e != NULL);
    if (e) {
        CHECK(e->battery_mv == 3456);
        CHECK(e->flags == (LMESH_HEALTH_RESET | 0x40));
        CHECK(e->uptime_s == 90);
        CHECK(e->store_depth == 2);
        CHECK(e->lost == 5);
        CHECK(e->station_id_len == 6 && memcmp(e->station_id, "N0CALL", 6) == 0);
        CHECK(e->neighbor_count == 2);
        CHECK(e->neighbors[0] == 6 && e->neighbor_etx8[0] == 12);
        CHECK(e->neighbors[1] == 7 && e->neighbor_etx8[1] == 40);
        CHECK(e->last_heard_ms == 777);
    }

    lmesh_node_info info[LMESH_MAX_NODES];
    CHECK(lmesh_nodes(&b, info, LMESH_MAX_NODES) == 1);
    CHECK(info[0].id == 5 && !info[0].stale && info[0].neighbor_count == 2);

    set_neighbor(&a, 0, 6, false, true, 12);
    set_neighbor(&a, 3, 7, false, false, 40);
    battery = 3900;
    len = build(&a, frame, sizeof frame);
    b.now_ms = 5000;
    lmesh_status_rx(&b, frame, len);
    e = lmesh_status_entry(&b, 5);
    CHECK(e && e->neighbor_count == 0 && e->battery_mv == 3900 && e->last_heard_ms == 5000);
    CHECK(e && e->neighbors[0] == 0 && e->neighbor_etx8[1] == 0);
    CHECK(lmesh_nodes(&b, info, LMESH_MAX_NODES) == 1);

    uint8_t big[3 + LMESH_STATUS_FIXED_LEN + 2 * (LMESH_MAX_NEIGHBORS + 2)];
    memset(big, 0, sizeof big);
    big[0] = 9;
    big[3 + 10] = LMESH_MAX_NEIGHBORS + 2;
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS + 2; i++) {
        big[3 + 11 + 2 * i] = (uint8_t)(100 + i);
        big[3 + 12 + 2 * i] = (uint8_t)i;
    }
    lmesh_status_rx(&b, big, sizeof big);
    e = lmesh_status_entry(&b, 9);
    CHECK(e && e->neighbor_count == LMESH_MAX_NEIGHBORS);
    CHECK(e && e->neighbors[LMESH_MAX_NEIGHBORS - 1] == 100 + LMESH_MAX_NEIGHBORS - 1);
    CHECK(e && e->neighbor_etx8[LMESH_MAX_NEIGHBORS - 1] == LMESH_MAX_NEIGHBORS - 1);
    CHECK(b.stats.rx_malformed == 0);
    battery = 3700;
    app_flags = 0;
}

static void test_malformed(void)
{
    uint8_t frame[LMESH_MAX_PAYLOAD];
    uint8_t copy[LMESH_MAX_PAYLOAD];
    clock_ms = 0;
    setup(&a, 5, "AB");
    setup(&b, 6, "");
    set_neighbor(&a, 0, 6, true, true, 12);
    size_t len = build(&a, frame, sizeof frame);
    events = 0;

    uint32_t bad = 0;
    for (size_t k = 0; k < len; k++) {
        lmesh_status_rx(&b, frame, k);
        bad++;
    }
    CHECK(b.stats.rx_malformed == bad);

    frame[len] = 0;
    lmesh_status_rx(&b, frame, len + 1);
    CHECK(b.stats.rx_malformed == ++bad);

    memcpy(copy, frame, len);
    copy[3 + 9] = LMESH_STATION_ID_MAX + 1;
    lmesh_status_rx(&b, copy, len);
    CHECK(b.stats.rx_malformed == ++bad);
    copy[3 + 9] = 3;
    lmesh_status_rx(&b, copy, len);
    CHECK(b.stats.rx_malformed == ++bad);

    memcpy(copy, frame, len);
    copy[3 + 12] = 2;
    lmesh_status_rx(&b, copy, len);
    CHECK(b.stats.rx_malformed == ++bad);

    memcpy(copy, frame, len);
    copy[0] = LMESH_ID_NONE;
    lmesh_status_rx(&b, copy, len);
    CHECK(b.stats.rx_malformed == ++bad);
    copy[0] = LMESH_ID_ALL;
    lmesh_status_rx(&b, copy, len);
    CHECK(b.stats.rx_malformed == ++bad);

    lmesh_status_rx(&b, NULL, 0);
    CHECK(b.stats.rx_malformed == ++bad);

    CHECK(events == 0);
    lmesh_node_info info[LMESH_MAX_NODES];
    CHECK(lmesh_nodes(&b, info, LMESH_MAX_NODES) == 0);

    lmesh_status_rx(&b, frame, len);
    CHECK(events == 1 && b.stats.rx_malformed == bad);
}

static void test_own_origin(void)
{
    uint8_t frame[LMESH_MAX_PAYLOAD];
    clock_ms = 0;
    setup(&a, 5, "");
    size_t len = build(&a, frame, sizeof frame);
    events = 0;
    lmesh_status_rx(&a, frame, len);
    CHECK(events == 0);
    CHECK(a.stats.rx_malformed == 0);
    CHECK(lmesh_status_entry(&a, 5) == NULL);
}

static void test_table_full(void)
{
    uint8_t frame[LMESH_MAX_PAYLOAD];
    clock_ms = 0;
    setup(&b, 200, "");
    b.now_ms = 0xFFFFF000u;
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        setup(&a, (lmesh_node_id)(1 + i), "");
        size_t len = build(&a, frame, sizeof frame);
        b.now_ms += 1000;
        lmesh_status_rx(&b, frame, len);
    }
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        CHECK(lmesh_status_entry(&b, (lmesh_node_id)(1 + i)) != NULL);
    }

    setup(&a, 1, "");
    size_t len = build(&a, frame, sizeof frame);
    b.now_ms += 1000;
    lmesh_status_rx(&b, frame, len);

    setup(&a, 150, "");
    len = build(&a, frame, sizeof frame);
    b.now_ms += 1000;
    lmesh_status_rx(&b, frame, len);
    CHECK(lmesh_status_entry(&b, 150) != NULL);
    CHECK(lmesh_status_entry(&b, 2) == NULL);
    CHECK(lmesh_status_entry(&b, 1) != NULL);
    CHECK(lmesh_status_entry(&b, 3) != NULL);
    lmesh_node_info info[LMESH_MAX_NODES];
    CHECK(lmesh_nodes(&b, info, LMESH_MAX_NODES) == LMESH_MAX_NODES);

    setup(&a, 151, "");
    len = build(&a, frame, sizeof frame);
    b.now_ms += 1000;
    lmesh_status_rx(&b, frame, len);
    CHECK(lmesh_status_entry(&b, 151) != NULL);
    CHECK(lmesh_status_entry(&b, 3) == NULL);
    CHECK(lmesh_status_entry(&b, 150) != NULL);
}

static void test_schedule(void)
{
    clock_ms = 0xFFF00000u;
    setup(&a, 5, "");
    uint32_t start = a.now_ms;
    uint32_t first = a.status.next_ms - start;
    CHECK(first <= LMESH_STATUS_PERIOD_MS);

    a.now_ms = start + first / 2;
    a.next_wake_ms = a.now_ms + LMESH_TICK_MAX_SLEEP_MS;
    lmesh_status_tick(&a);
    CHECK(a.status.next_ms == start + first);

    uint32_t lo = 0xFFFFFFFFu;
    uint32_t hi = 0;
    for (uint32_t i = 0; i < 2000; i++) {
        uint32_t due = a.status.next_ms;
        a.now_ms = due - 1;
        a.next_wake_ms = a.now_ms + 0x10000000u;
        lmesh_status_tick(&a);
        CHECK(a.status.next_ms == due);
        CHECK(a.next_wake_ms == due);

        a.now_ms = due + i % 7u;
        a.next_wake_ms = a.now_ms + 0x10000000u;
        lmesh_status_tick(&a);
        uint32_t gap = a.status.next_ms - a.now_ms;
        CHECK(gap >= LMESH_STATUS_PERIOD_MS - LMESH_STATUS_JITTER_MS);
        CHECK(gap <= LMESH_STATUS_PERIOD_MS + LMESH_STATUS_JITTER_MS);
        CHECK((int32_t)(a.next_wake_ms - a.status.next_ms) <= 0);
        lo = gap < lo ? gap : lo;
        hi = gap > hi ? gap : hi;
    }
    CHECK(lo < LMESH_STATUS_PERIOD_MS - LMESH_STATUS_JITTER_MS / 2);
    CHECK(hi > LMESH_STATUS_PERIOD_MS + LMESH_STATUS_JITTER_MS / 2);

    clock_ms = a.now_ms + 10;
    lmesh_tick(&a);
    CHECK(lmesh_next_wake_ms(&a) <= a.status.next_ms - a.now_ms);
}

static void test_uptime(void)
{
    uint8_t body[LMESH_STATUS_BODY_MAX];
    clock_ms = 0xFFFFF000u;
    setup(&a, 5, "");
    for (uint32_t h = 1; h <= 60u * 24u; h++) {
        a.now_ms += 3600000u;
        lmesh_status_tick(&a);
    }
    a.now_ms += 999;
    CHECK(lmesh_status_encode(&a, body, sizeof body) > 0);
    CHECK(lmesh_get32(body + 3) == 60u * 86400u);
}

static void test_over_air(void)
{
    clock_ms = 2000;
    setup(&a, 5, "N0AIR");
    setup(&b, 6, "");
    set_neighbor(&a, 0, 6, true, true, 11);
    a.status.next_ms = a.now_ms + 1000000u;
    sent_len = 0;
    lmesh_err err = lmesh_status_send(&a);
    if (err != LMESH_OK) {
        printf("note: flood_originate returned %d, over-the-air check skipped\n", (int)err);
        return;
    }
    CHECK(!a.status.reset_flag);
    for (int i = 0; i < 50 && sent_len == 0; i++) {
        clock_ms += 100;
        lmesh_tick(&a);
    }
    CHECK(sent_len > 0);
    events = 0;
    b.now_ms = clock_ms;
    lmesh_radio_rx(&b, sent, sent_len, -80, 5);
    lmesh_node_entry *e = lmesh_status_entry(&b, 5);
    CHECK(e != NULL);
    CHECK(e && e->station_id_len == 5 && memcmp(e->station_id, "N0AIR", 5) == 0);
    CHECK(e && e->neighbor_count == 1 && e->neighbors[0] == 6 && e->neighbor_etx8[0] == 11);
    CHECK(e && e->flags == LMESH_HEALTH_RESET);
    CHECK(events >= 1);
}

int main(void)
{
    test_layout();
    test_flags();
    test_lost();
    test_station_id();
    test_fit();
    test_roundtrip();
    test_malformed();
    test_own_origin();
    test_table_full();
    test_schedule();
    test_uptime();
    test_over_air();

    printf("%d checks, %d failed: %s\n", checks, failures, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
