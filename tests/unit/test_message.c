/* Unit tests for the message layer: submit, header, holding, scheduler, link results, relay, delivery, receipts, cancel. */
#include <stdio.h>
#include <string.h>

#include "link.h"
#include "message.h"
#include "store.h"

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

#define ME 1
#define NB 2
#define NB2 3
#define FAR 4
#define MAX_CAP 512
#define EVENT_MAX 512
#define NO_ACK (-1)

static uint32_t clock_ms;
static uint32_t rnd_state;
static bool on_air;
static int ncap;
static struct {
    uint8_t buf[LMESH_MAX_FRAME];
    size_t len;
} cap[MAX_CAP];
static int depth_at_ack;

static lmesh_event events[EVENT_MAX];
static uint8_t rx_data[EVENT_MAX][LMESH_MAX_CONTENT];
static int event_count;

static int cmd_calls;
static lmesh_node_id cmd_from;
static uint8_t cmd_data[LMESH_MAX_CONTENT];
static size_t cmd_len;
static uint8_t cmd_code;

static lmesh_node node;
static uint8_t frame_seq;

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
    on_air = true;
    if ((frame[0] & 0x0Fu) == LMESH_FT_HOP_ACK) {
        depth_at_ack = lmesh_store_depth(&node);
    }
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
    if (event_count < EVENT_MAX) {
        events[event_count] = *ev;
        if (ev->type == LMESH_EV_MSG_RECEIVED && ev->u.msg_received.data) {
            memcpy(rx_data[event_count], ev->u.msg_received.data, ev->u.msg_received.len);
        }
    }
    event_count++;
}

static void on_command(void *ctx, lmesh_node_id from, const uint8_t *data, size_t len, lmesh_cmd_result *result)
{
    (void)ctx;
    cmd_calls++;
    cmd_from = from;
    cmd_len = len;
    memcpy(cmd_data, data, len);
    result->code = cmd_code;
    result->len = 2;
    result->data[0] = 0xAA;
    result->data[1] = 0xBB;
}

static void boot(void)
{
    lmesh_identity ident = {.id = ME, .network_id = 0x5A};
    for (unsigned i = 0; i < 16; i++) {
        ident.key[i] = (uint8_t)(0x30u + i);
    }
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    lmesh_callbacks cb = {.on_event = on_event, .on_command = on_command};
    clock_ms = 1000;
    rnd_state = 777;
    on_air = false;
    ncap = 0;
    cmd_calls = 0;
    cmd_code = LMESH_RESULT_EXECUTED;
    frame_seq = 0;
    CHECK(lmesh_init(&node, &ident, &port, &cb) == LMESH_OK);
    event_count = 0;
}

static void add_neighbor(lmesh_node_id id)
{
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        lmesh_neighbor *n = &node.nbr.table[i];
        if (!n->used) {
            memset(n, 0, sizeof *n);
            n->used = true;
            n->up = true;
            n->bidirectional = true;
            n->id = id;
            n->etx8 = 8;
            n->rx_quality = 255;
            n->tx_quality = 255;
            n->last_heard_ms = clock_ms;
            n->last_hello_ms = clock_ms;
            n->tx_quality_ms = clock_ms;
            return;
        }
    }
}

static void add_route(lmesh_node_id dest, lmesh_node_id via)
{
    for (size_t i = 0; i < LMESH_MAX_NODES; i++) {
        lmesh_route *r = &node.route.table[i];
        if (!r->used) {
            memset(r, 0, sizeof *r);
            r->used = true;
            r->dest = dest;
            r->next_hop = via;
            r->metric8 = 16;
            r->seqno = 1;
            r->refreshed_ms = clock_ms;
            r->last_req_ms = clock_ms;
            return;
        }
    }
}

static void set_time(uint32_t t)
{
    clock_ms = t;
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        lmesh_neighbor *n = &node.nbr.table[i];
        if (n->used && n->up) {
            n->last_hello_ms = t;
            n->last_heard_ms = t;
            n->fail_count = 0;
        }
    }
}

static void complete(void)
{
    if (on_air) {
        on_air = false;
        lmesh_radio_tx_done(&node, true);
    }
}

static uint8_t cap_type(int i)
{
    return (uint8_t)(cap[i].buf[0] & 0x0Fu);
}

static const uint8_t *cap_payload(int i)
{
    return cap[i].buf + LMESH_HEADER_LEN;
}

static int step(void)
{
    for (int k = 0; k < 32; k++) {
        complete();
        int before = ncap;
        lmesh_tick(&node);
        if (ncap == before) {
            return -1;
        }
        if (cap_type(ncap - 1) == LMESH_FT_DATA) {
            on_air = false;
            lmesh_radio_tx_done(&node, true);
            return ncap - 1;
        }
    }
    return -1;
}

static void deliver_frame(uint8_t type, lmesh_node_id tx, uint8_t seq, uint8_t flags, const uint8_t *payload,
                          size_t len)
{
    uint8_t buf[LMESH_MAX_FRAME];
    lmesh_frame_hdr h = {.type = type, .tx = tx, .rx = ME, .seq = seq, .flags = flags, .payload_len = (uint8_t)len};
    size_t n = lmesh_frame_encode(&node, &h, payload, buf, sizeof buf);
    CHECK(n > 0);
    lmesh_radio_rx(&node, buf, n, -80, 5);
}

static void ack(int f, uint8_t status)
{
    uint8_t payload[2] = {cap[f].buf[4], status};
    deliver_frame(LMESH_FT_HOP_ACK, cap[f].buf[3], frame_seq++, 0, payload, sizeof payload);
}

static int data_from(lmesh_node_id tx, const uint8_t *payload, size_t len)
{
    complete();
    int before = ncap;
    depth_at_ack = -1;
    deliver_frame(LMESH_FT_DATA, tx, frame_seq++, LMESH_FLAG_ACK_REQ, payload, len);
    int status = NO_ACK;
    for (int i = before; i < ncap && i < MAX_CAP; i++) {
        if (cap_type(i) == LMESH_FT_HOP_ACK && cap[i].buf[3] == tx) {
            status = cap_payload(i)[1];
        }
    }
    complete();
    return status;
}

static size_t make_msg(uint8_t *out, lmesh_node_id origin, uint16_t id, lmesh_node_id dest, uint8_t type_prio,
                       uint32_t lifetime_s, uint8_t hops, const void *content, size_t len)
{
    out[0] = origin;
    lmesh_put16(out + 1, id);
    out[3] = dest;
    out[4] = type_prio;
    lmesh_put32(out + 5, lifetime_s);
    out[9] = hops;
    if (len > 0) {
        memcpy(out + LMESH_MSG_HEADER_LEN, content, len);
    }
    return LMESH_MSG_HEADER_LEN + len;
}

static uint8_t tp(lmesh_msg_type type, lmesh_prio prio)
{
    return (uint8_t)((unsigned)type | ((unsigned)prio << LMESH_MSG_PRIO_SHIFT));
}

static lmesh_record *rec(lmesh_node_id origin, uint16_t id)
{
    return lmesh_store_find(&node, origin, id, 0xFFu);
}

static int idx_of(const lmesh_record *r)
{
    return (int)(r - node.store.records);
}

static int count_events(lmesh_event_type type)
{
    int n = 0;
    for (int i = 0; i < event_count && i < EVENT_MAX; i++) {
        n += events[i].type == type ? 1 : 0;
    }
    return n;
}

static const lmesh_event *last_event(lmesh_event_type type)
{
    for (int i = (event_count < EVENT_MAX ? event_count : EVENT_MAX) - 1; i >= 0; i--) {
        if (events[i].type == type) {
            return &events[i];
        }
    }
    return NULL;
}

static bool last_state_is(uint16_t id, lmesh_msg_state state, lmesh_fail_reason reason)
{
    const lmesh_event *ev = last_event(LMESH_EV_MSG_STATE);
    return ev && ev->u.msg_state.ref.origin == ME && ev->u.msg_state.ref.id == id && ev->u.msg_state.state == state &&
           ev->u.msg_state.reason == reason;
}

static int count_receipts(void)
{
    int n = 0;
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        const lmesh_record *r = &node.store.records[i];
        n += r->role == LMESH_ROLE_RELAY && r->origin == ME && lmesh_record_type(r) == LMESH_MSG_RECEIPT ? 1 : 0;
    }
    return n;
}

static const lmesh_record *newest_receipt(void)
{
    const lmesh_record *best = NULL;
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        const lmesh_record *r = &node.store.records[i];
        if (r->role == LMESH_ROLE_RELAY && r->origin == ME && lmesh_record_type(r) == LMESH_MSG_RECEIPT &&
            (!best || r->id > best->id)) {
            best = r;
        }
    }
    return best;
}

static int link_queued(void)
{
    int n = 0;
    for (int i = 0; i < LMESH_TXQ_SIZE; i++) {
        n += node.link.queue[i].used && node.link.queue[i].owner == LMESH_OWNER_MESSAGE ? 1 : 0;
    }
    return n;
}

static void fail_hop(void)
{
    for (int k = 0; k < 200 && node.msg.in_flight >= 0; k++) {
        clock_ms += 250;
        (void)step();
    }
}

static lmesh_msg_ref submit(lmesh_node_id dest, lmesh_msg_type type, lmesh_prio prio, uint32_t lifetime_s)
{
    lmesh_msg_ref ref = {0, 0};
    CHECK(lmesh_send(&node, dest, type, prio, lifetime_s, (const uint8_t *)"hello", 5, &ref) == LMESH_OK);
    return ref;
}

static void test_submit(void)
{
    boot();
    lmesh_msg_ref ref = {0, 0};
    CHECK(lmesh_send(&node, FAR, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0, (const uint8_t *)"hi", 2, &ref) == LMESH_OK);
    CHECK(ref.origin == ME && ref.id == 1);
    CHECK(event_count == 1 && events[0].type == LMESH_EV_MSG_STATE);
    CHECK(events[0].u.msg_state.state == LMESH_STATE_QUEUED && events[0].u.msg_state.reason == LMESH_REASON_NONE);
    CHECK(events[0].u.msg_state.dest == FAR && events[0].u.msg_state.prio == LMESH_PRIO_NORMAL);
    lmesh_record *r = rec(ME, 1);
    CHECK(r && r->role == LMESH_ROLE_ORIGIN_COPY && r->state == LMESH_STATE_QUEUED);
    CHECK(r && r->lifetime_s == LMESH_LIFETIME_TEXT_S && r->hops == 0 && r->content_len == 2);
    CHECK(r && r->type_prio == tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL) && memcmp(r->content, "hi", 2) == 0);
    CHECK(r && lmesh_record_valid(r));
    CHECK(lmesh_next_wake_ms(&node) == 0);

    lmesh_msg_ref c = submit(FAR, LMESH_MSG_COMMAND, LMESH_PRIO_NORMAL, 0);
    lmesh_msg_ref t = submit(FAR, LMESH_MSG_TELEMETRY, LMESH_PRIO_ROUTINE, 0);
    lmesh_msg_ref e = submit(FAR, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, 77);
    CHECK(c.id == 2 && t.id == 3 && e.id == 4);
    CHECK(rec(ME, 2)->lifetime_s == LMESH_LIFETIME_COMMAND_S);
    CHECK(rec(ME, 3)->lifetime_s == LMESH_LIFETIME_TEXT_S);
    CHECK(rec(ME, 4)->lifetime_s == 77);

    lmesh_msg_info info[8];
    CHECK(lmesh_messages(&node, info, 8) == 4);
    CHECK(info[0].state == LMESH_STATE_QUEUED && info[0].role == LMESH_ROLE_ORIGIN_COPY && info[0].attempts == 0);

    CHECK(lmesh_message_submit(&node, FAR, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0, NULL, 3, &ref) == LMESH_ERR_ARG);
    CHECK(lmesh_message_submit(&node, FAR, LMESH_MSG_TEXT, LMESH_PRIO_COUNT, 0, NULL, 0, &ref) == LMESH_ERR_ARG);

    int ok = 4;
    while (ok < LMESH_STORE_SIZE + 4 &&
           lmesh_send(&node, FAR, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, 0, NULL, 0, &ref) == LMESH_OK) {
        ok++;
    }
    CHECK(lmesh_store_depth(&node) == LMESH_STORE_SIZE);
    CHECK(lmesh_send(&node, FAR, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, 0, NULL, 0, &ref) == LMESH_ERR_FULL);
    CHECK(rec(ME, 3) == NULL);
}

static void test_header_and_hold(void)
{
    boot();
    lmesh_msg_ref ref = submit(FAR, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    CHECK(step() == -1);
    CHECK(step() == -1);
    lmesh_record *r = rec(ME, ref.id);
    CHECK(r && r->attempts == 0 && r->in_flight_to == LMESH_ID_NONE && node.msg.in_flight == -1);

    set_time(clock_ms + 5500);
    add_neighbor(NB2);
    add_route(FAR, NB2);
    node.next_wake_ms = node.now_ms + 100000u;
    lmesh_message_route_event(&node, FAR);
    CHECK(lmesh_next_wake_ms(&node) == 0);

    int f = step();
    CHECK(f >= 0);
    lmesh_frame_hdr h;
    const uint8_t *p = NULL;
    CHECK(f >= 0 && lmesh_frame_decode(&node, cap[f].buf, cap[f].len, &h, &p) == LMESH_OK);
    CHECK(h.type == LMESH_FT_DATA && h.tx == ME && h.rx == NB2 && (h.flags & LMESH_FLAG_ACK_REQ));
    CHECK(h.payload_len == LMESH_MSG_HEADER_LEN + 5);
    CHECK(p && p[0] == ME && lmesh_get16(p + 1) == ref.id && p[3] == FAR);
    CHECK(p && p[4] == tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL));
    CHECK(p && lmesh_get32(p + 5) == LMESH_LIFETIME_TEXT_S - 5u);
    CHECK(p && p[9] == 1 && memcmp(p + LMESH_MSG_HEADER_LEN, "hello", 5) == 0);
    CHECK(r->in_flight_to == NB2 && node.msg.in_flight == idx_of(r) && r->attempts == 0);
    CHECK(step() == -1);

    boot();
    add_neighbor(NB);
    ref = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    f = step();
    CHECK(f >= 0 && cap[f].buf[3] == NB);

    boot();
    add_neighbor(NB);
    node.nbr.table[0].bidirectional = false;
    submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    CHECK(step() == -1);
    node.nbr.table[0].bidirectional = true;
    lmesh_message_route_event(&node, NB);
    CHECK(step() >= 0);

    boot();
    submit(FAR, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 2);
    set_time(clock_ms + 2100);
    add_neighbor(NB2);
    add_route(FAR, NB2);
    CHECK(step() == -1);
    CHECK(last_state_is(1, LMESH_STATE_EXPIRED, LMESH_REASON_LIFETIME));
}

static void test_link_results(void)
{
    boot();
    add_neighbor(NB);
    lmesh_msg_ref ref = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 4000);
    lmesh_record *r = rec(ME, ref.id);
    int f = step();
    CHECK(f >= 0);
    CHECK((cap_payload(f)[4] & LMESH_MSG_FLAG_RESEND) == 0);
    int before = count_events(LMESH_EV_MSG_STATE);
    ack(f, LMESH_ACK_ACCEPTED);
    CHECK(node.msg.in_flight == -1);
    CHECK(r->role == LMESH_ROLE_ORIGIN_SHADOW && r->state == LMESH_STATE_TRANSMITTED && r->in_flight_to == 0);
    CHECK(count_events(LMESH_EV_MSG_STATE) == before + 1);
    CHECK(last_state_is(ref.id, LMESH_STATE_TRANSMITTED, LMESH_REASON_HOP_ACCEPTED));
    CHECK(r->e2e_backoff_ms == LMESH_E2E_RETRY_MIN_MS);
    CHECK(r->next_attempt_ms == node.now_ms + LMESH_E2E_RETRY_MIN_MS);
    CHECK(lmesh_record_valid(r));

    uint32_t fire = r->next_attempt_ms;
    set_time(fire - 1);
    CHECK(step() == -1);
    set_time(fire);
    f = step();
    CHECK(f >= 0 && (cap_payload(f)[4] & LMESH_MSG_FLAG_RESEND) != 0);
    CHECK(f >= 0 && (cap_payload(f)[4] & (LMESH_MSG_TYPE_MASK | LMESH_MSG_PRIO_MASK)) ==
                        tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL));
    CHECK(r->state == LMESH_STATE_QUEUED && r->role == LMESH_ROLE_ORIGIN_SHADOW);
    CHECK(last_state_is(ref.id, LMESH_STATE_QUEUED, LMESH_REASON_RESEND));
    ack(f, LMESH_ACK_DUPLICATE);
    CHECK(last_state_is(ref.id, LMESH_STATE_TRANSMITTED, LMESH_REASON_HOP_ACCEPTED));
    CHECK(r->e2e_backoff_ms == 2u * LMESH_E2E_RETRY_MIN_MS);
    CHECK(r->next_attempt_ms == node.now_ms + 2u * LMESH_E2E_RETRY_MIN_MS);
    for (int k = 0; k < 6; k++) {
        set_time(r->next_attempt_ms);
        f = step();
        CHECK(f >= 0);
        ack(f, LMESH_ACK_ACCEPTED);
    }
    CHECK(r->e2e_backoff_ms == LMESH_E2E_RETRY_MAX_MS);

    boot();
    add_neighbor(NB);
    ref = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    r = rec(ME, ref.id);
    f = step();
    before = count_events(LMESH_EV_MSG_STATE);
    ack(f, LMESH_ACK_REFUSED_FULL);
    uint32_t t0 = node.now_ms;
    CHECK(r->role == LMESH_ROLE_ORIGIN_COPY && r->state == LMESH_STATE_QUEUED && r->attempts == 0);
    CHECK(r->next_attempt_ms == t0 + LMESH_REFUSE_BACKOFF_MS);
    CHECK(count_events(LMESH_EV_MSG_STATE) == before);
    CHECK(node.stats.link_failures == 0 && node.nbr.table[0].fail_count == 0);
    node.now_ms = t0 + 4500u;
    node.next_wake_ms = node.now_ms + 100000u;
    lmesh_message_tick(&node);
    CHECK(node.next_wake_ms == t0 + LMESH_REFUSE_BACKOFF_MS);
    CHECK(node.msg.in_flight == -1 && link_queued() == 0);
    set_time(t0 + LMESH_REFUSE_BACKOFF_MS - 1);
    CHECK(step() == -1);
    set_time(t0 + LMESH_REFUSE_BACKOFF_MS);
    f = step();
    CHECK(f >= 0);

    fail_hop();
    CHECK(node.msg.in_flight == -1 && node.stats.link_failures == 1);
    CHECK(r->attempts == 1 && r->role == LMESH_ROLE_ORIGIN_COPY);
    uint32_t wait = r->next_attempt_ms - node.now_ms;
    CHECK(wait >= LMESH_FWD_BACKOFF_MIN_MS && wait <= LMESH_FWD_BACKOFF_MIN_MS + LMESH_FWD_BACKOFF_MIN_MS / 4u);
    set_time(r->next_attempt_ms);
    f = step();
    CHECK(f >= 0);
    fail_hop();
    CHECK(r->attempts == 2);
    wait = r->next_attempt_ms - node.now_ms;
    CHECK(wait >= 2u * LMESH_FWD_BACKOFF_MIN_MS && wait <= 2u * LMESH_FWD_BACKOFF_MIN_MS * 5u / 4u);
    for (int k = 0; k < 6; k++) {
        set_time(r->next_attempt_ms);
        CHECK(step() >= 0);
        fail_hop();
    }
    CHECK(r->attempts == 8);
    wait = r->next_attempt_ms - node.now_ms;
    CHECK(wait == LMESH_FWD_BACKOFF_MAX_MS);
    CHECK(r->state == LMESH_STATE_QUEUED && lmesh_record_valid(r));

    boot();
    add_neighbor(NB);
    ref = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    f = step();
    ack(f, LMESH_ACK_REFUSED_INVALID);
    r = rec(ME, ref.id);
    CHECK(r && r->role == LMESH_ROLE_HISTORY && r->state == LMESH_STATE_FAILED);
    CHECK(last_state_is(ref.id, LMESH_STATE_FAILED, LMESH_REASON_INVALID));
    CHECK(lmesh_store_depth(&node) == 0 && step() == -1);
}

static void test_wrr(void)
{
    boot();
    add_neighbor(NB);
    for (int i = 0; i < 12; i++) {
        submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, 0);
    }
    submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_ROUTINE, 0);
    int routine_at = -1;
    for (int k = 0; k < 13; k++) {
        int f = step();
        CHECK(f >= 0);
        if (f < 0) {
            break;
        }
        if (((cap_payload(f)[4] & LMESH_MSG_PRIO_MASK) >> LMESH_MSG_PRIO_SHIFT) == LMESH_PRIO_ROUTINE) {
            routine_at = k;
        }
        ack(f, LMESH_ACK_ACCEPTED);
    }
    CHECK(routine_at >= 0 && routine_at <= 6);

    boot();
    add_neighbor(NB);
    for (int i = 0; i < 12; i++) {
        submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, 0);
        submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    }
    for (int i = 0; i < 3; i++) {
        submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_ROUTINE, 0);
    }
    int routine_pos[3] = {-1, -1, -1};
    int nr = 0;
    int run = 0;
    int max_run = 0;
    uint16_t last_id[LMESH_PRIO_COUNT] = {0, 0, 0};
    bool ordered = true;
    for (int k = 0; k < 21; k++) {
        int f = step();
        CHECK(f >= 0);
        if (f < 0) {
            break;
        }
        unsigned prio = (cap_payload(f)[4] & LMESH_MSG_PRIO_MASK) >> LMESH_MSG_PRIO_SHIFT;
        uint16_t id = lmesh_get16(cap_payload(f) + 1);
        ordered = ordered && id > last_id[prio];
        last_id[prio] = id;
        if (prio == LMESH_PRIO_ROUTINE) {
            if (nr < 3) {
                routine_pos[nr++] = k;
            }
            run = 0;
        } else if (++run > max_run) {
            max_run = run;
        }
        ack(f, LMESH_ACK_ACCEPTED);
    }
    CHECK(routine_pos[0] == 6 && routine_pos[1] == 13 && routine_pos[2] == 20);
    CHECK(max_run <= 6);
    CHECK(ordered);
}

static void test_relay(void)
{
    uint8_t m[LMESH_MAX_PAYLOAD];
    boot();
    add_neighbor(NB);
    add_neighbor(NB2);
    add_route(FAR, NB2);

    size_t n = make_msg(m, NB, 50, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, 1, "abc", 3);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(depth_at_ack == 1);
    lmesh_record *r = rec(NB, 50);
    CHECK(r && r->role == LMESH_ROLE_RELAY && r->hops == 1 && r->lifetime_s == 300 && r->dest == FAR);
    CHECK(r && r->content_len == 3 && memcmp(r->content, "abc", 3) == 0 && lmesh_record_valid(r));
    CHECK(count_events(LMESH_EV_MSG_STATE) == 0);

    CHECK(data_from(NB, m, n) == LMESH_ACK_DUPLICATE);
    CHECK(node.stats.dup_relay == 1 && lmesh_store_depth(&node) == 1);

    set_time(clock_ms + 2500);
    int f = step();
    CHECK(f >= 0 && cap[f].buf[3] == NB2);
    CHECK(f >= 0 && cap_payload(f)[0] == NB && lmesh_get16(cap_payload(f) + 1) == 50);
    CHECK(f >= 0 && cap_payload(f)[9] == 2 && lmesh_get32(cap_payload(f) + 5) == 298);
    CHECK(f >= 0 && (cap_payload(f)[4] & LMESH_MSG_FLAG_RESEND) == 0);
    ack(f, LMESH_ACK_ACCEPTED);
    CHECK(rec(NB, 50) == NULL && lmesh_store_depth(&node) == 0);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 0);

    m[4] = (uint8_t)(m[4] | LMESH_MSG_FLAG_RESEND);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(node.stats.dup_relay == 1 && lmesh_store_depth(&node) == 1);
    set_time(clock_ms + 2500);
    f = step();
    CHECK(f >= 0 && cap[f].buf[3] == NB2 && (cap_payload(f)[4] & LMESH_MSG_FLAG_RESEND) != 0);
    ack(f, LMESH_ACK_ACCEPTED);
    CHECK(rec(NB, 50) == NULL && lmesh_store_depth(&node) == 0 && count_events(LMESH_EV_MSG_STATE) == 0);

    n = make_msg(m, NB, 51, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, LMESH_MAX_HOPS, "x", 1);
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    CHECK(node.stats.hop_limit_drops == 1 && rec(NB, 51) == NULL);
    n = make_msg(m, NB, 51, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, LMESH_MAX_HOPS - 1, "x", 1);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    f = step();
    CHECK(f >= 0 && cap_payload(f)[9] == LMESH_MAX_HOPS);
    ack(f, LMESH_ACK_TIMEOUT);
    ack(f, LMESH_ACK_REFUSED_INVALID);
    CHECK(rec(NB, 51) == NULL && count_events(LMESH_EV_MSG_STATE) == 0);

    n = make_msg(m, ME, 52, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, 2, "x", 1);
    CHECK(data_from(NB, m, n) == LMESH_ACK_DUPLICATE);
    CHECK(rec(ME, 52) == NULL);

    n = make_msg(m, NB, 53, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_ROUTINE), 300, 1, "y", 1);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    f = step();
    ack(f, LMESH_ACK_REFUSED_FULL);
    r = rec(NB, 53);
    CHECK(r && r->role == LMESH_ROLE_RELAY && r->next_attempt_ms == node.now_ms + LMESH_REFUSE_BACKOFF_MS);
    CHECK(data_from(NB, m, n) == LMESH_ACK_DUPLICATE);

    boot();
    add_neighbor(NB);
    lmesh_msg_ref ref;
    while (lmesh_send(&node, FAR, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, 0, NULL, 0, &ref) == LMESH_OK) {
    }
    CHECK(lmesh_store_depth(&node) == LMESH_STORE_SIZE);
    n = make_msg(m, NB, 60, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, 1, "z", 1);
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_FULL);
    CHECK(rec(NB, 60) == NULL);
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_FULL);
}

static void test_malformed(void)
{
    uint8_t m[LMESH_MAX_PAYLOAD];
    boot();
    add_neighbor(NB);
    size_t n = make_msg(m, NB, 70, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, 1, NULL, 0);
    uint32_t bad = node.stats.rx_malformed;

    CHECK(data_from(NB, m, LMESH_MSG_HEADER_LEN - 1) == LMESH_ACK_REFUSED_INVALID);
    CHECK(data_from(NB, m, 0) == LMESH_ACK_REFUSED_INVALID);
    m[4] = 5;
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    m[4] = (uint8_t)(3u << LMESH_MSG_PRIO_SHIFT);
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    m[4] = tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL);
    m[3] = LMESH_ID_ALL;
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    m[3] = LMESH_ID_NONE;
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    m[3] = FAR;
    m[0] = LMESH_ID_NONE;
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    m[0] = FAR;
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    m[0] = NB;
    m[9] = 0;
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    m[9] = 1;
    lmesh_put32(m + 5, 0);
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    lmesh_put32(m + 5, 300);

    uint8_t rc[2] = {1, 0};
    n = make_msg(m, NB, 71, ME, tp(LMESH_MSG_RECEIPT, LMESH_PRIO_NORMAL), 300, 1, rc, sizeof rc);
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    uint8_t rc_bad[3] = {1, 0, 9};
    n = make_msg(m, NB, 71, ME, tp(LMESH_MSG_RECEIPT, LMESH_PRIO_NORMAL), 300, 1, rc_bad, sizeof rc_bad);
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);

    CHECK(node.stats.rx_malformed == bad + 12);
    CHECK(lmesh_store_depth(&node) == 0 && count_events(LMESH_EV_MSG_RECEIVED) == 0);

    n = make_msg(m, NB, 72, FAR, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, 1, NULL, 0);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(node.stats.rx_malformed == bad + 12);
}

static void test_destination(void)
{
    uint8_t m[LMESH_MAX_PAYLOAD];
    boot();
    add_neighbor(NB);

    size_t n = make_msg(m, NB, 7, ME, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, 1, "hi", 2);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(depth_at_ack == 1);
    CHECK(count_events(LMESH_EV_MSG_RECEIVED) == 1);
    const lmesh_event *ev = last_event(LMESH_EV_MSG_RECEIVED);
    int ev_at = (int)(ev - events);
    CHECK(ev && ev->u.msg_received.ref.origin == NB && ev->u.msg_received.ref.id == 7);
    CHECK(ev && ev->u.msg_received.msg_type == LMESH_MSG_TEXT && ev->u.msg_received.prio == LMESH_PRIO_NORMAL);
    CHECK(ev && ev->u.msg_received.hops == 1 && ev->u.msg_received.len == 2 && memcmp(rx_data[ev_at], "hi", 2) == 0);
    CHECK(node.stats.delivered == 1);
    const lmesh_record *rc = newest_receipt();
    CHECK(count_receipts() == 1);
    CHECK(rc && rc->dest == NB && rc->type_prio == tp(LMESH_MSG_RECEIPT, LMESH_PRIO_NORMAL));
    CHECK(rc && rc->lifetime_s == LMESH_LIFETIME_TEXT_S && rc->hops == 0 && rc->content_len == 3);
    CHECK(rc && lmesh_get16(rc->content) == 7 && rc->content[2] == LMESH_RESULT_DELIVERED);
    uint16_t first_receipt_id = rc ? rc->id : 0;

    CHECK(data_from(NB, m, n) == LMESH_ACK_DUPLICATE);
    CHECK(count_events(LMESH_EV_MSG_RECEIVED) == 1 && node.stats.dup_dest == 1 && node.stats.delivered == 1);
    CHECK(count_receipts() == 2);
    rc = newest_receipt();
    CHECK(rc && rc->id != first_receipt_id && lmesh_get16(rc->content) == 7);

    for (int k = 0; k < 2; k++) {
        int f = step();
        CHECK(f >= 0 && cap[f].buf[3] == NB);
        const uint8_t *p = cap_payload(f);
        CHECK(f >= 0 && p[0] == ME && p[3] == NB && (p[4] & LMESH_MSG_TYPE_MASK) == LMESH_MSG_RECEIPT);
        CHECK(f >= 0 && p[9] == 1 && lmesh_get16(p + LMESH_MSG_HEADER_LEN) == 7);
        ack(f, LMESH_ACK_ACCEPTED);
    }
    CHECK(count_receipts() == 0 && lmesh_store_depth(&node) == 0);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 0);

    n = make_msg(m, NB, 8, ME, tp(LMESH_MSG_COMMAND, LMESH_PRIO_URGENT), 100, 2, "go", 2);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(cmd_calls == 1 && cmd_from == NB && cmd_len == 2 && memcmp(cmd_data, "go", 2) == 0);
    rc = newest_receipt();
    CHECK(rc && rc->type_prio == tp(LMESH_MSG_RECEIPT, LMESH_PRIO_URGENT) && rc->content_len == 5);
    CHECK(rc && lmesh_get16(rc->content) == 8 && rc->content[2] == LMESH_RESULT_EXECUTED);
    CHECK(rc && rc->content[3] == 0xAA && rc->content[4] == 0xBB);
    CHECK(data_from(NB, m, n) == LMESH_ACK_DUPLICATE);
    CHECK(cmd_calls == 1 && count_events(LMESH_EV_MSG_RECEIVED) == 2);
    rc = newest_receipt();
    CHECK(rc && lmesh_get16(rc->content) == 8 && rc->content[2] == LMESH_RESULT_EXECUTED && rc->content_len == 3);

    cmd_code = LMESH_RESULT_REJECTED;
    n = make_msg(m, NB, 9, ME, tp(LMESH_MSG_COMMAND, LMESH_PRIO_NORMAL), 100, 1, "no", 2);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(cmd_calls == 2 && newest_receipt()->content[2] == LMESH_RESULT_REJECTED);
    CHECK(data_from(NB, m, n) == LMESH_ACK_DUPLICATE);
    CHECK(cmd_calls == 2 && newest_receipt()->content[2] == LMESH_RESULT_REJECTED);

    node.cb.on_command = NULL;
    n = make_msg(m, NB, 10, ME, tp(LMESH_MSG_COMMAND, LMESH_PRIO_NORMAL), 100, 1, "x", 1);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(cmd_calls == 2 && newest_receipt()->content[2] == LMESH_RESULT_UNSUPPORTED);
    CHECK(newest_receipt()->content_len == 3);

    n = make_msg(m, NB, 11, ME, tp(LMESH_MSG_TELEMETRY, LMESH_PRIO_ROUTINE), 100, LMESH_MAX_HOPS, "t", 1);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(newest_receipt()->type_prio == tp(LMESH_MSG_RECEIPT, LMESH_PRIO_ROUTINE));
    n = make_msg(m, NB, 12, ME, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 100, LMESH_MAX_HOPS + 1, "t", 1);
    uint32_t drops = node.stats.hop_limit_drops;
    CHECK(data_from(NB, m, n) == LMESH_ACK_REFUSED_INVALID);
    CHECK(node.stats.hop_limit_drops == drops + 1 && count_events(LMESH_EV_MSG_RECEIVED) == 5);

    boot();
    add_neighbor(NB);
    lmesh_msg_ref ref;
    while (lmesh_send(&node, FAR, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, 0, NULL, 0, &ref) == LMESH_OK) {
    }
    n = make_msg(m, NB, 20, ME, tp(LMESH_MSG_TEXT, LMESH_PRIO_NORMAL), 300, 1, "full", 4);
    CHECK(data_from(NB, m, n) == LMESH_ACK_ACCEPTED);
    CHECK(count_events(LMESH_EV_MSG_RECEIVED) == 1 && count_receipts() == 0);
}

static void send_receipt(lmesh_node_id from, uint16_t rid, uint16_t orig_id, uint8_t code, int want)
{
    uint8_t m[LMESH_MAX_PAYLOAD];
    uint8_t c[3];
    lmesh_put16(c, orig_id);
    c[2] = code;
    size_t n = make_msg(m, from, rid, ME, tp(LMESH_MSG_RECEIPT, LMESH_PRIO_NORMAL), 300, 1, c, sizeof c);
    CHECK(data_from(from, m, n) == want);
}

static void test_receipt_at_origin(void)
{
    boot();
    add_neighbor(NB);
    lmesh_msg_ref a = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    int f = step();
    ack(f, LMESH_ACK_ACCEPTED);
    lmesh_record *r = rec(ME, a.id);
    CHECK(r && r->role == LMESH_ROLE_ORIGIN_SHADOW);

    int states = count_events(LMESH_EV_MSG_STATE);
    send_receipt(NB, 100, a.id, LMESH_RESULT_DELIVERED, LMESH_ACK_ACCEPTED);
    CHECK(count_events(LMESH_EV_MSG_STATE) == states + 1);
    CHECK(last_state_is(a.id, LMESH_STATE_DELIVERED, LMESH_REASON_RECEIPT));
    CHECK(last_event(LMESH_EV_MSG_STATE)->u.msg_state.result_code == LMESH_RESULT_DELIVERED);
    CHECK(r->role == LMESH_ROLE_HISTORY && r->state == LMESH_STATE_DELIVERED);
    CHECK(node.stats.delivered == 1 && lmesh_store_depth(&node) == 0 && count_receipts() == 0);

    send_receipt(NB, 100, a.id, LMESH_RESULT_DELIVERED, LMESH_ACK_DUPLICATE);
    CHECK(node.stats.dup_dest == 1 && count_events(LMESH_EV_MSG_STATE) == states + 1);
    send_receipt(NB, 101, a.id, LMESH_RESULT_DELIVERED, LMESH_ACK_ACCEPTED);
    CHECK(count_events(LMESH_EV_MSG_STATE) == states + 1 && node.stats.delivered == 1);
    send_receipt(NB, 102, 999, LMESH_RESULT_DELIVERED, LMESH_ACK_ACCEPTED);
    CHECK(count_events(LMESH_EV_MSG_STATE) == states + 1 && count_events(LMESH_EV_MSG_RECEIVED) == 0);

    lmesh_msg_ref b = submit(NB, LMESH_MSG_COMMAND, LMESH_PRIO_URGENT, 0);
    f = step();
    ack(f, LMESH_ACK_ACCEPTED);
    send_receipt(NB, 103, b.id, LMESH_RESULT_REJECTED, LMESH_ACK_ACCEPTED);
    CHECK(last_state_is(b.id, LMESH_STATE_FAILED, LMESH_REASON_REJECTED));
    CHECK(last_event(LMESH_EV_MSG_STATE)->u.msg_state.result_code == LMESH_RESULT_REJECTED);

    lmesh_msg_ref c = submit(NB, LMESH_MSG_COMMAND, LMESH_PRIO_URGENT, 0);
    f = step();
    ack(f, LMESH_ACK_ACCEPTED);
    send_receipt(NB, 104, c.id, LMESH_RESULT_EXECUTED, LMESH_ACK_ACCEPTED);
    CHECK(last_state_is(c.id, LMESH_STATE_DELIVERED, LMESH_REASON_RECEIPT));
    CHECK(last_event(LMESH_EV_MSG_STATE)->u.msg_state.result_code == LMESH_RESULT_EXECUTED);

    lmesh_msg_ref d = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    node.now_ms = clock_ms;
    lmesh_message_tick(&node);
    r = rec(ME, d.id);
    CHECK(node.msg.in_flight == idx_of(r) && link_queued() == 1);
    send_receipt(NB, 105, d.id, LMESH_RESULT_UNSUPPORTED, LMESH_ACK_ACCEPTED);
    CHECK(node.msg.in_flight == -1 && link_queued() == 0);
    CHECK(last_state_is(d.id, LMESH_STATE_FAILED, LMESH_REASON_REJECTED));
    CHECK(step() == -1);

    lmesh_msg_ref e = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    f = step();
    CHECK(f >= 0);
    send_receipt(NB, 106, e.id, LMESH_RESULT_DELIVERED, LMESH_ACK_ACCEPTED);
    CHECK(last_state_is(e.id, LMESH_STATE_DELIVERED, LMESH_REASON_RECEIPT));
    CHECK(node.msg.in_flight >= 0);
    states = count_events(LMESH_EV_MSG_STATE);
    ack(f, LMESH_ACK_ACCEPTED);
    CHECK(node.msg.in_flight == -1 && count_events(LMESH_EV_MSG_STATE) == states);
    CHECK(rec(ME, e.id)->role == LMESH_ROLE_HISTORY);
}

static void test_cancel(void)
{
    boot();
    add_neighbor(NB);
    lmesh_msg_ref bogus = {ME, 4321};
    CHECK(lmesh_cancel(&node, bogus) == LMESH_ERR_NOT_FOUND);
    lmesh_msg_ref foreign = {NB, 1};
    CHECK(lmesh_cancel(&node, foreign) == LMESH_ERR_NOT_FOUND);

    lmesh_msg_ref a = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    CHECK(lmesh_cancel(&node, a) == LMESH_OK);
    CHECK(last_state_is(a.id, LMESH_STATE_FAILED, LMESH_REASON_CANCELED));
    CHECK(rec(ME, a.id)->role == LMESH_ROLE_HISTORY);
    CHECK(lmesh_cancel(&node, a) == LMESH_ERR_STATE);
    CHECK(step() == -1);

    lmesh_msg_ref b = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    node.now_ms = clock_ms;
    lmesh_message_tick(&node);
    CHECK(node.msg.in_flight >= 0 && link_queued() == 1);
    CHECK(lmesh_cancel(&node, b) == LMESH_OK);
    CHECK(node.msg.in_flight == -1 && link_queued() == 0);
    CHECK(last_state_is(b.id, LMESH_STATE_FAILED, LMESH_REASON_CANCELED));
    CHECK(step() == -1);

    lmesh_msg_ref c = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    int f = step();
    CHECK(f >= 0);
    CHECK(lmesh_cancel(&node, c) == LMESH_ERR_STATE);
    CHECK(rec(ME, c.id)->role == LMESH_ROLE_ORIGIN_COPY);
    ack(f, LMESH_ACK_ACCEPTED);
    CHECK(lmesh_cancel(&node, c) == LMESH_ERR_STATE);
    CHECK(rec(ME, c.id)->role == LMESH_ROLE_ORIGIN_SHADOW);

    lmesh_msg_ref d = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    f = step();
    fail_hop();
    CHECK(rec(ME, d.id)->attempts == 1);
    CHECK(lmesh_cancel(&node, d) == LMESH_OK);

    lmesh_msg_ref e = submit(FAR, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    CHECK(step() == -1);
    CHECK(lmesh_cancel(&node, e) == LMESH_OK);
    CHECK(lmesh_store_depth(&node) == 1);
}

static void test_robustness(void)
{
    boot();
    add_neighbor(NB);
    lmesh_msg_ref a = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    int f = step();
    lmesh_record *r = rec(ME, a.id);
    r->crc = (uint16_t)(r->crc ^ 1u);
    int states = count_events(LMESH_EV_MSG_STATE);
    ack(f, LMESH_ACK_ACCEPTED);
    CHECK(node.msg.in_flight == -1 && count_events(LMESH_EV_MSG_STATE) == states);

    lmesh_message_link_result(&node, LMESH_STORE_SIZE, LMESH_ACK_ACCEPTED);
    lmesh_message_link_result(&node, 0xFFFFu, LMESH_ACK_TIMEOUT);
    CHECK(node.msg.in_flight == -1);

    boot();
    add_neighbor(NB);
    lmesh_msg_ref b = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    node.now_ms = clock_ms;
    lmesh_message_tick(&node);
    r = rec(ME, b.id);
    CHECK(node.msg.in_flight == idx_of(r));
    lmesh_store_release(&node, r);
    lmesh_message_tick(&node);
    CHECK(node.msg.in_flight == -1 && link_queued() == 0);

    lmesh_msg_ref c = submit(NB, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 0);
    f = step();
    CHECK(f >= 0);
    r = rec(ME, c.id);
    lmesh_store_release(&node, r);
    lmesh_message_tick(&node);
    CHECK(node.msg.in_flight >= 0);
    ack(f, LMESH_ACK_ACCEPTED);
    CHECK(node.msg.in_flight == -1 && lmesh_store_depth(&node) == 0);
}

int main(void)
{
    test_submit();
    test_header_and_hold();
    test_link_results();
    test_wrr();
    test_relay();
    test_malformed();
    test_destination();
    test_receipt_at_origin();
    test_cancel();
    test_robustness();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
