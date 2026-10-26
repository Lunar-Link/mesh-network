/* Unit tests for the link layer: transmit queue, weighted round-robin, hop acknowledgment, retries, repeats. */
#include <stdio.h>
#include <string.h>

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

#define MAX_CAP 128
#define SLOT (15u + 2u * LMESH_MAX_FRAME)

static uint32_t clock_ms;
static uint32_t rnd_state;
static int send_result;
static int busy_left;
static int busy_calls;
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
    if (send_result != 0) {
        return send_result;
    }
    if (ncap < MAX_CAP) {
        memcpy(cap[ncap].buf, frame, len);
        cap[ncap].len = len;
    }
    ncap++;
    return 0;
}

static bool radio_busy(void *ctx)
{
    (void)ctx;
    busy_calls++;
    if (busy_left > 0) {
        busy_left--;
        return true;
    }
    return false;
}

static uint32_t airtime_ms(void *ctx, size_t len)
{
    (void)ctx;
    return 15u + (uint32_t)len * 2u;
}

static lmesh_node node;
static lmesh_node peer;
static lmesh_node other;

static void make_node(lmesh_node *n, lmesh_node_id id, bool with_busy)
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
    n->port.radio_busy = with_busy ? radio_busy : NULL;
    n->port.airtime_ms = airtime_ms;
    n->link.in_flight = -1;
    n->now_ms = clock_ms;
    n->next_wake_ms = clock_ms + LMESH_TICK_MAX_SLEEP_MS;
}

static void setup(bool with_busy)
{
    clock_ms = 1000;
    rnd_state = 12345;
    send_result = 0;
    busy_left = 0;
    busy_calls = 0;
    ncap = 0;
    make_node(&node, 1, with_busy);
    make_node(&peer, 2, false);
    make_node(&other, 3, false);
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

static lmesh_err send1(uint8_t band, uint32_t due, bool want_ack, uint8_t tag)
{
    uint8_t payload[4] = {tag, band, 0xAB, 0xCD};
    lmesh_node_id to = want_ack ? peer.ident.id : (lmesh_node_id)LMESH_ID_ALL;
    uint8_t type = want_ack ? (uint8_t)LMESH_FT_DATA : (uint8_t)LMESH_FT_HELLO;
    return lmesh_link_send(&node, type, to, payload, sizeof payload, band, due, want_ack, LMESH_OWNER_NONE, 0);
}

static bool decode_cap(int i, lmesh_frame_hdr *h, const uint8_t **p)
{
    return lmesh_frame_decode(&peer, cap[i].buf, cap[i].len, h, p) == LMESH_OK;
}

static uint8_t cap_tag(int i)
{
    return cap[i].buf[LMESH_HEADER_LEN];
}

static void deliver_ack(lmesh_node *from, uint8_t seq, uint8_t status, uint8_t len)
{
    uint8_t payload[2] = {seq, status};
    uint8_t buf[LMESH_MAX_FRAME];
    lmesh_frame_hdr h = {.type = LMESH_FT_HOP_ACK, .tx = from->ident.id, .rx = 1, .seq = 77, .payload_len = len};
    size_t n = lmesh_frame_encode(from, &h, payload, buf, sizeof buf);
    lmesh_frame_hdr d;
    const uint8_t *p = NULL;
    CHECK(lmesh_frame_decode(&node, buf, n, &d, &p) == LMESH_OK);
    lmesh_link_rx_ack(&node, &d, p, d.payload_len);
}

static void test_send_args(void)
{
    uint8_t big[LMESH_MAX_PAYLOAD + 1] = {0};
    setup(false);
    CHECK(lmesh_link_send(&node, LMESH_FT_HELLO, LMESH_ID_ALL, big, 1, LMESH_PRIO_COUNT, 0, false, 0, 0) ==
          LMESH_ERR_ARG);
    CHECK(lmesh_link_send(&node, LMESH_FT_HELLO, LMESH_ID_ALL, big, sizeof big, LMESH_PRIO_ROUTINE, 0, false, 0,
                          0) == LMESH_ERR_TOO_LONG);
    CHECK(lmesh_link_send(&node, LMESH_FT_DATA, LMESH_ID_ALL, big, 1, LMESH_PRIO_ROUTINE, 0, true, 0, 0) ==
          LMESH_ERR_ARG);
    CHECK(lmesh_link_send(&node, LMESH_FT_HELLO, LMESH_ID_ALL, NULL, 1, LMESH_PRIO_ROUTINE, 0, false, 0, 0) ==
          LMESH_ERR_ARG);
    CHECK(lmesh_link_send(&node, 16, LMESH_ID_ALL, big, 1, LMESH_PRIO_ROUTINE, 0, false, 0, 0) == LMESH_ERR_ARG);
    CHECK(queued() == 0 && node.link.frame_seq == 0);

    CHECK(lmesh_link_send(&node, LMESH_FT_HELLO, LMESH_ID_ALL, big, LMESH_MAX_PAYLOAD, LMESH_PRIO_ROUTINE, 1500, false,
                          LMESH_OWNER_HELLO, 9) == LMESH_OK);
    CHECK(node.next_wake_ms == 1500);
    for (int i = 1; i < LMESH_TXQ_SIZE; i++) {
        CHECK(send1(LMESH_PRIO_NORMAL, 2000, false, (uint8_t)i) == LMESH_OK);
    }
    CHECK(send1(LMESH_PRIO_NORMAL, 2000, false, 99) == LMESH_ERR_FULL);
    CHECK(queued() == LMESH_TXQ_SIZE);
    CHECK(node.link.frame_seq == LMESH_TXQ_SIZE);
    const lmesh_txq_entry *e = &node.link.queue[0];
    CHECK(e->owner == LMESH_OWNER_HELLO && e->owner_ref == 9 && e->band == LMESH_PRIO_ROUTINE && e->attempts == 0);
    CHECK(e->frame.len == LMESH_MAX_FRAME && e->frame.buf[2] == 1 && e->frame.buf[3] == LMESH_ID_ALL);
    CHECK(e->frame.buf[4] == 0 && e->frame.buf[5] == 0);
    CHECK(node.link.queue[1].frame.buf[4] == 1);

    CHECK(lmesh_link_cancel(&node, LMESH_OWNER_HELLO, 9));
    CHECK(!lmesh_link_cancel(&node, LMESH_OWNER_HELLO, 9));
    CHECK(queued() == LMESH_TXQ_SIZE - 1);
}

static void test_due_order(void)
{
    setup(false);
    CHECK(send1(LMESH_PRIO_NORMAL, 1300, false, 3) == LMESH_OK);
    CHECK(send1(LMESH_PRIO_NORMAL, 1100, false, 1) == LMESH_OK);
    CHECK(send1(LMESH_PRIO_NORMAL, 1200, false, 2) == LMESH_OK);
    tick();
    CHECK(ncap == 0);
    CHECK(node.next_wake_ms == 1100);

    clock_ms = 1100;
    tick();
    CHECK(ncap == 1 && cap_tag(0) == 1);
    CHECK(node.link.tx_active && node.link.in_flight >= 0);
    CHECK(node.stats.tx_frames[LMESH_FT_HELLO] == 1);
    CHECK(node.stats.airtime_ms == airtime_ms(NULL, cap[0].len));
    lmesh_link_tx_done(&node, true);
    CHECK(queued() == 2 && node.link.in_flight == -1 && !node.link.tx_active);
    CHECK(node.next_wake_ms == clock_ms);
    tick();
    CHECK(ncap == 1);
    CHECK(node.next_wake_ms == 1200);

    clock_ms = 1400;
    tick();
    CHECK(ncap == 2 && cap_tag(1) == 2);
    lmesh_link_tx_done(&node, true);
    tick();
    CHECK(ncap == 3 && cap_tag(2) == 3);
    lmesh_link_tx_done(&node, false);
    CHECK(queued() == 0);
    CHECK(node.stats.ack_timeouts == 0 && node.stats.link_failures == 0);
}

static void test_one_in_flight(void)
{
    setup(false);
    CHECK(send1(LMESH_PRIO_URGENT, 1000, false, 1) == LMESH_OK);
    CHECK(send1(LMESH_PRIO_URGENT, 1000, false, 2) == LMESH_OK);
    tick();
    tick();
    CHECK(ncap == 1);
    lmesh_link_tx_done(&node, true);
    lmesh_link_tx_done(&node, true);
    CHECK(queued() == 1);
    tick();
    CHECK(ncap == 2);
    lmesh_link_tx_done(&node, true);

    CHECK(send1(LMESH_PRIO_URGENT, 1000, true, 3) == LMESH_OK);
    CHECK(send1(LMESH_PRIO_URGENT, 1000, false, 4) == LMESH_OK);
    tick();
    CHECK(ncap == 3 && cap_tag(2) == 3);
    lmesh_link_tx_done(&node, true);
    CHECK(node.link.awaiting_ack && !node.link.tx_active);
    clock_ms += 50;
    tick();
    CHECK(ncap == 3);
    deliver_ack(&peer, cap[2].buf[4], LMESH_ACK_ACCEPTED, 2);
    CHECK(!node.link.awaiting_ack && node.link.in_flight == -1);
    tick();
    CHECK(ncap == 4 && cap_tag(3) == 4);
}

static void wrr_run(int transmissions, bool normal_load, int *routine_wait_max, int counts[LMESH_PRIO_COUNT])
{
    int since_routine_queued = 0;
    for (int b = 0; b < LMESH_PRIO_COUNT; b++) {
        counts[b] = 0;
    }
    *routine_wait_max = 0;
    for (int t = 0; t < transmissions; t++) {
        int per_band[LMESH_PRIO_COUNT] = {0};
        for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
            if (node.link.queue[i].used) {
                per_band[node.link.queue[i].band]++;
            }
        }
        while (per_band[LMESH_PRIO_URGENT]++ < 3) {
            CHECK(send1(LMESH_PRIO_URGENT, clock_ms, false, 0) == LMESH_OK);
        }
        while (normal_load && per_band[LMESH_PRIO_NORMAL]++ < 3) {
            CHECK(send1(LMESH_PRIO_NORMAL, clock_ms, false, 0) == LMESH_OK);
        }
        if (per_band[LMESH_PRIO_ROUTINE] == 0) {
            CHECK(send1(LMESH_PRIO_ROUTINE, clock_ms, false, 0) == LMESH_OK);
            since_routine_queued = 0;
        }
        int before = ncap;
        tick();
        CHECK(ncap == before + 1);
        uint8_t band = cap[(ncap - 1) % MAX_CAP].buf[LMESH_HEADER_LEN + 1];
        counts[band]++;
        since_routine_queued++;
        if (band == LMESH_PRIO_ROUTINE && since_routine_queued > *routine_wait_max) {
            *routine_wait_max = since_routine_queued;
        }
        lmesh_link_tx_done(&node, true);
        clock_ms += 10;
    }
}

static void test_wrr(void)
{
    int wait_max = 0;
    int counts[LMESH_PRIO_COUNT];
    const int w = LMESH_WRR_URGENT + LMESH_WRR_NORMAL + LMESH_WRR_ROUTINE;

    setup(false);
    wrr_run(10 * w, true, &wait_max, counts);
    CHECK(wait_max <= w);
    CHECK(counts[LMESH_PRIO_URGENT] == 10 * LMESH_WRR_URGENT);
    CHECK(counts[LMESH_PRIO_NORMAL] == 10 * LMESH_WRR_NORMAL);
    CHECK(counts[LMESH_PRIO_ROUTINE] == 10 * LMESH_WRR_ROUTINE);

    setup(false);
    wrr_run(10 * (LMESH_WRR_URGENT + LMESH_WRR_ROUTINE), false, &wait_max, counts);
    CHECK(wait_max <= LMESH_WRR_URGENT + LMESH_WRR_ROUTINE);
    CHECK(counts[LMESH_PRIO_ROUTINE] == 10 && counts[LMESH_PRIO_NORMAL] == 0);

    setup(false);
    for (int i = 0; i < 5; i++) {
        CHECK(send1(LMESH_PRIO_URGENT, clock_ms, false, (uint8_t)i) == LMESH_OK);
    }
    CHECK(send1(LMESH_PRIO_ROUTINE, clock_ms, false, 0xEE) == LMESH_OK);
    int pos = -1;
    for (int t = 0; t < 6; t++) {
        tick();
        if (cap_tag(ncap - 1) == 0xEE) {
            pos = t;
        }
        lmesh_link_tx_done(&node, true);
    }
    CHECK(pos == LMESH_WRR_URGENT);
}

static int run_to_send(void)
{
    int before = ncap;
    for (int guard = 0; guard < 100 && ncap == before; guard++) {
        tick();
        if (ncap == before) {
            clock_ms = node.next_wake_ms;
        }
    }
    return ncap - before;
}

static void test_ack_retry(void)
{
    const uint32_t slot = SLOT;
    setup(false);
    CHECK(lmesh_link_slot_ms(&node) == slot);
    CHECK(send1(LMESH_PRIO_NORMAL, 1000, true, 5) == LMESH_OK);
    tick();
    CHECK(ncap == 1);
    lmesh_frame_hdr h0;
    const uint8_t *p0;
    CHECK(decode_cap(0, &h0, &p0));
    CHECK(h0.type == LMESH_FT_DATA && h0.rx == 2 && h0.tx == 1 && h0.flags == LMESH_FLAG_ACK_REQ);

    clock_ms = 1010;
    lmesh_link_tx_done(&node, true);
    uint32_t deadline = 1010 + airtime_ms(NULL, cap[0].len) + airtime_ms(NULL, 14) + LMESH_ACK_GUARD_MS;
    CHECK(node.link.awaiting_ack && node.link.ack_deadline_ms == deadline);
    tick();
    CHECK(node.next_wake_ms == deadline);
    clock_ms = deadline - 1;
    tick();
    CHECK(node.stats.ack_timeouts == 0 && node.link.awaiting_ack);

    for (unsigned k = 1; k <= LMESH_HOP_RETRIES; k++) {
        clock_ms = node.link.ack_deadline_ms;
        tick();
        CHECK(node.stats.ack_timeouts == k);
        CHECK(!node.link.awaiting_ack && node.link.in_flight == -1);
        const lmesh_txq_entry *e = &node.link.queue[0];
        CHECK(e->used && e->attempts == k);
        uint32_t back = e->due_ms - clock_ms;
        CHECK(back % slot == 0 && back >= slot && back <= (1u << k) * slot);
        CHECK(node.next_wake_ms == e->due_ms);
        clock_ms = e->due_ms - 1;
        tick();
        CHECK(ncap == (int)k);
        clock_ms = e->due_ms;
        tick();
        CHECK(ncap == (int)k + 1);
        lmesh_frame_hdr h;
        const uint8_t *p;
        CHECK(decode_cap(ncap - 1, &h, &p));
        CHECK(h.flags == (LMESH_FLAG_ACK_REQ | LMESH_FLAG_RETRY));
        CHECK(h.seq == h0.seq && h.payload_len == h0.payload_len && memcmp(p, p0, h.payload_len) == 0);
        lmesh_link_tx_done(&node, true);
    }
    CHECK(node.stats.link_failures == 0);
    clock_ms = node.link.ack_deadline_ms;
    tick();
    CHECK(node.stats.ack_timeouts == LMESH_HOP_RETRIES + 1u);
    CHECK(node.stats.link_failures == 1);
    CHECK(queued() == 0 && node.link.in_flight == -1 && !node.link.awaiting_ack);
    CHECK(ncap == LMESH_HOP_RETRIES + 1);
    CHECK(node.stats.tx_frames[LMESH_FT_DATA] == LMESH_HOP_RETRIES + 1u);
}

static void test_backoff_bounds(void)
{
    const uint32_t slot = SLOT;
    uint32_t seen_min[LMESH_HOP_RETRIES + 1];
    uint32_t seen_max[LMESH_HOP_RETRIES + 1];
    for (unsigned k = 0; k <= LMESH_HOP_RETRIES; k++) {
        seen_min[k] = 0xFFFFFFFFu;
        seen_max[k] = 0;
    }
    for (unsigned trial = 0; trial < 200; trial++) {
        setup(false);
        rnd_state = trial * 7919u + 1u;
        CHECK(send1(LMESH_PRIO_NORMAL, 1000, true, 1) == LMESH_OK);
        for (unsigned k = 1; k <= LMESH_HOP_RETRIES; k++) {
            CHECK(run_to_send() == 1);
            lmesh_link_tx_done(&node, k % 2 == 0 ? false : true);
            if (k % 2 == 1) {
                clock_ms = node.link.ack_deadline_ms;
                tick();
            }
            CHECK(node.link.queue[0].attempts == k);
            uint32_t back = node.link.queue[0].due_ms - clock_ms;
            CHECK(back % slot == 0);
            back /= slot;
            seen_min[k] = back < seen_min[k] ? back : seen_min[k];
            seen_max[k] = back > seen_max[k] ? back : seen_max[k];
        }
    }
    for (unsigned k = 1; k <= LMESH_HOP_RETRIES; k++) {
        CHECK(seen_min[k] == 1);
        CHECK(seen_max[k] == (1u << k));
    }
}

static void test_tx_failure(void)
{
    setup(false);
    CHECK(send1(LMESH_PRIO_NORMAL, 1000, true, 1) == LMESH_OK);
    tick();
    lmesh_link_tx_done(&node, false);
    CHECK(node.stats.ack_timeouts == 1 && node.link.queue[0].attempts == 1);
    CHECK(!node.link.awaiting_ack && node.link.in_flight == -1);
    CHECK((node.link.queue[0].frame.buf[5] & LMESH_FLAG_RETRY) != 0);

    setup(false);
    send_result = -1;
    CHECK(send1(LMESH_PRIO_NORMAL, 1000, false, 1) == LMESH_OK);
    tick();
    CHECK(ncap == 0 && !node.link.tx_active && node.link.in_flight == -1);
    CHECK(node.stats.tx_frames[LMESH_FT_HELLO] == 0);
    CHECK(node.next_wake_ms == clock_ms + 1);
    send_result = 0;
    clock_ms += 1;
    tick();
    CHECK(ncap == 1 && node.link.tx_active);
}

static void test_ack_matching(void)
{
    setup(false);
    CHECK(send1(LMESH_PRIO_NORMAL, 1000, true, 1) == LMESH_OK);
    tick();
    uint8_t seq = cap[0].buf[4];
    deliver_ack(&peer, seq, LMESH_ACK_ACCEPTED, 2);
    CHECK(node.link.in_flight >= 0);
    lmesh_link_tx_done(&node, true);

    deliver_ack(&other, seq, LMESH_ACK_ACCEPTED, 2);
    CHECK(node.link.awaiting_ack);
    deliver_ack(&peer, (uint8_t)(seq - 1u), LMESH_ACK_ACCEPTED, 2);
    CHECK(node.link.awaiting_ack);
    deliver_ack(&peer, seq, LMESH_ACK_ACCEPTED, 1);
    CHECK(node.link.awaiting_ack);
    deliver_ack(&peer, seq, 7, 2);
    CHECK(node.link.awaiting_ack);
    deliver_ack(&peer, seq, LMESH_ACK_REFUSED_FULL, 2);
    CHECK(!node.link.awaiting_ack && node.link.in_flight == -1 && queued() == 0);
    CHECK(node.stats.ack_timeouts == 0 && node.stats.link_failures == 0);
    deliver_ack(&peer, seq, LMESH_ACK_ACCEPTED, 2);
    CHECK(queued() == 0 && node.link.in_flight == -1);
}

static void test_send_ack(void)
{
    lmesh_frame_hdr h;
    const uint8_t *p;

    setup(false);
    lmesh_link_send_ack(&node, 2, 41, LMESH_ACK_ACCEPTED);
    CHECK(ncap == 1 && node.link.tx_active && node.link.ack_on_air && !node.link.ack_pending);
    CHECK(decode_cap(0, &h, &p));
    CHECK(h.type == LMESH_FT_HOP_ACK && h.tx == 1 && h.rx == 2 && h.payload_len == 2 && p[0] == 41 &&
          p[1] == LMESH_ACK_ACCEPTED && h.flags == 0);
    CHECK(cap[0].len == LMESH_HOP_ACK_FRAME_LEN);
    CHECK(node.stats.tx_frames[LMESH_FT_HOP_ACK] == 1);
    CHECK(send1(LMESH_PRIO_URGENT, 1000, false, 9) == LMESH_OK);
    tick();
    CHECK(ncap == 1);
    lmesh_link_tx_done(&node, true);
    CHECK(!node.link.ack_on_air && !node.link.tx_active);
    tick();
    CHECK(ncap == 2 && cap_tag(1) == 9);

    lmesh_link_send_ack(&node, 3, 42, LMESH_ACK_DUPLICATE);
    CHECK(ncap == 2 && node.link.ack_pending);
    CHECK(send1(LMESH_PRIO_URGENT, 1000, false, 10) == LMESH_OK);
    lmesh_link_tx_done(&node, true);
    tick();
    CHECK(ncap == 3 && node.link.ack_on_air);
    CHECK(decode_cap(2, &h, &p) && h.type == LMESH_FT_HOP_ACK && h.rx == 3 && p[0] == 42);
    lmesh_link_tx_done(&node, true);
    tick();
    CHECK(ncap == 4 && cap_tag(3) == 10);
    lmesh_link_tx_done(&node, true);

    CHECK(send1(LMESH_PRIO_URGENT, 1000, true, 11) == LMESH_OK);
    tick();
    CHECK(ncap == 5);
    lmesh_link_tx_done(&node, true);
    CHECK(node.link.awaiting_ack);
    uint8_t data_seq = cap[4].buf[4];
    lmesh_link_send_ack(&node, 3, 43, LMESH_ACK_REFUSED_FULL);
    CHECK(ncap == 6 && node.link.ack_on_air && node.link.awaiting_ack);
    lmesh_link_tx_done(&node, true);
    CHECK(node.link.awaiting_ack && node.link.in_flight >= 0);
    deliver_ack(&peer, data_seq, LMESH_ACK_ACCEPTED, 2);
    CHECK(!node.link.awaiting_ack && queued() == 0);

    CHECK(node.link.last_rx[0].id == 2 && node.link.last_rx[0].last_seq == 41);
    CHECK(node.link.last_rx[1].id == 3 && node.link.last_rx[1].last_seq == 42);
    CHECK(node.link.last_rx[2].id == LMESH_ID_NONE);
}

static lmesh_frame_hdr data_hdr(lmesh_node *from, uint8_t seq, uint8_t flags)
{
    uint8_t payload[3] = {1, 2, 3};
    uint8_t buf[LMESH_MAX_FRAME];
    lmesh_frame_hdr h = {
        .type = LMESH_FT_DATA, .tx = from->ident.id, .rx = 1, .seq = seq, .flags = flags, .payload_len = 3};
    size_t n = lmesh_frame_encode(from, &h, payload, buf, sizeof buf);
    lmesh_frame_hdr d = {0};
    const uint8_t *p;
    CHECK(lmesh_frame_decode(&node, buf, n, &d, &p) == LMESH_OK);
    return d;
}

static void test_repeat(void)
{
    const uint8_t retry = LMESH_FLAG_ACK_REQ | LMESH_FLAG_RETRY;
    setup(false);
    lmesh_frame_hdr h = data_hdr(&peer, 7, retry);
    CHECK(!lmesh_link_is_repeat(&node, &h));
    lmesh_link_send_ack(&node, 2, 7, LMESH_ACK_ACCEPTED);
    CHECK(lmesh_link_is_repeat(&node, &h));
    h = data_hdr(&peer, 7, LMESH_FLAG_ACK_REQ);
    CHECK(!lmesh_link_is_repeat(&node, &h));
    h = data_hdr(&peer, 8, retry);
    CHECK(!lmesh_link_is_repeat(&node, &h));
    h = data_hdr(&other, 7, retry);
    CHECK(!lmesh_link_is_repeat(&node, &h));

    lmesh_link_tx_done(&node, true);
    lmesh_link_send_ack(&node, 2, 8, LMESH_ACK_DUPLICATE);
    h = data_hdr(&peer, 8, retry);
    CHECK(lmesh_link_is_repeat(&node, &h));
    h = data_hdr(&peer, 7, retry);
    CHECK(!lmesh_link_is_repeat(&node, &h));

    lmesh_link_tx_done(&node, true);
    lmesh_link_send_ack(&node, 2, 9, LMESH_ACK_REFUSED_FULL);
    h = data_hdr(&peer, 9, retry);
    CHECK(!lmesh_link_is_repeat(&node, &h));
    h = data_hdr(&peer, 8, retry);
    CHECK(lmesh_link_is_repeat(&node, &h));

    lmesh_link_neighbor_reset(&node, 2);
    CHECK(!lmesh_link_is_repeat(&node, &h));

    for (unsigned i = 0; i <= LMESH_MAX_NEIGHBORS; i++) {
        lmesh_link_tx_done(&node, true);
        lmesh_link_send_ack(&node, (lmesh_node_id)(10u + i), (uint8_t)i, LMESH_ACK_ACCEPTED);
    }
    lmesh_frame_hdr q = {.tx = 10, .seq = 0, .flags = retry};
    CHECK(!lmesh_link_is_repeat(&node, &q));
    for (unsigned i = 1; i <= LMESH_MAX_NEIGHBORS; i++) {
        q.tx = (lmesh_node_id)(10u + i);
        q.seq = (uint8_t)i;
        CHECK(lmesh_link_is_repeat(&node, &q));
    }
}

static void test_busy(void)
{
    const uint32_t slot = SLOT;
    setup(true);
    busy_left = 1;
    CHECK(send1(LMESH_PRIO_NORMAL, 1000, false, 1) == LMESH_OK);
    tick();
    CHECK(ncap == 0 && busy_calls == 1 && !node.link.tx_active);
    uint32_t back = node.link.queue[0].due_ms - 1000;
    CHECK(back % slot == 0 && back >= slot && back <= 4 * slot);
    CHECK(node.next_wake_ms == node.link.queue[0].due_ms);
    tick();
    CHECK(ncap == 0 && busy_calls == 1);
    clock_ms = node.link.queue[0].due_ms;
    tick();
    CHECK(ncap == 1 && busy_calls == 2);

    uint32_t seen_min = 99, seen_max = 0;
    for (unsigned trial = 0; trial < 100; trial++) {
        setup(true);
        rnd_state = trial * 31u + 5u;
        busy_left = 1;
        CHECK(send1(LMESH_PRIO_NORMAL, 1000, false, 1) == LMESH_OK);
        tick();
        uint32_t k = (node.link.queue[0].due_ms - 1000) / slot;
        seen_min = k < seen_min ? k : seen_min;
        seen_max = k > seen_max ? k : seen_max;
    }
    CHECK(seen_min == 1 && seen_max == 4);

    setup(false);
    CHECK(send1(LMESH_PRIO_NORMAL, 1000, false, 1) == LMESH_OK);
    tick();
    CHECK(ncap == 1 && busy_calls == 0);
}

int main(void)
{
    test_send_args();
    test_due_order();
    test_one_in_flight();
    test_wrr();
    test_ack_retry();
    test_backoff_bounds();
    test_tx_failure();
    test_ack_matching();
    test_send_ack();
    test_repeat();
    test_busy();

    printf("%d checks, %d failures\n", checks, failures);
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
