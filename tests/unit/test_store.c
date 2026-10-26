/* Unit tests for the message store: allocation, eviction order, lifetime, history, checksum, message-id persistence. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

#define SLOTS 4
#define SLOT_MAX 512
#define EVENT_MAX 256

static uint32_t clock_ms;
static uint8_t slot_buf[SLOTS][SLOT_MAX];
static size_t slot_len[SLOTS];
static int slot_writes[SLOTS];

static lmesh_event events[EVENT_MAX];
static int event_count;

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    return 4;
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
    if (slot >= SLOTS || slot_len[slot] != len) {
        return -1;
    }
    memcpy(buf, slot_buf[slot], len);
    return 0;
}

static int storage_write(void *ctx, uint8_t slot, const void *buf, size_t len)
{
    (void)ctx;
    if (slot >= SLOTS || len > SLOT_MAX) {
        return -1;
    }
    memcpy(slot_buf[slot], buf, len);
    slot_len[slot] = len;
    slot_writes[slot]++;
    return 0;
}

static void on_event(void *ctx, const lmesh_event *ev)
{
    (void)ctx;
    if (event_count < EVENT_MAX) {
        events[event_count] = *ev;
    }
    event_count++;
}

static lmesh_node node;

static void boot(bool storage)
{
    lmesh_identity ident = {.id = 7, .network_id = 1};
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    if (storage) {
        port.storage_read = storage_read;
        port.storage_write = storage_write;
    }
    lmesh_callbacks cb = {.on_event = on_event};
    CHECK(lmesh_init(&node, &ident, &port, &cb) == LMESH_OK);
    event_count = 0;
}

static void wipe_storage(void)
{
    memset(slot_buf, 0, sizeof slot_buf);
    memset(slot_len, 0, sizeof slot_len);
    memset(slot_writes, 0, sizeof slot_writes);
}

static void set_time(uint32_t t)
{
    clock_ms = t;
    node.now_ms = t;
}

static void set_prio(lmesh_record *r, lmesh_prio prio)
{
    r->type_prio = (uint8_t)(LMESH_MSG_TEXT | ((unsigned)prio << 4));
    lmesh_record_seal(r);
}

static lmesh_record *put(uint8_t role, lmesh_prio prio, uint32_t lifetime_s, lmesh_node_id origin, uint16_t id)
{
    lmesh_record *r = lmesh_store_alloc(&node, prio, role);
    if (!r) {
        return NULL;
    }
    r->origin = origin;
    r->dest = 9;
    r->id = id;
    r->lifetime_s = lifetime_s;
    set_prio(r, prio);
    return r;
}

static int count_role(uint8_t role)
{
    int n = 0;
    for (size_t i = 0; i < LMESH_STORE_SIZE; i++) {
        n += node.store.records[i].role == role;
    }
    return n;
}

static int count_events(lmesh_event_type type)
{
    int n = 0;
    for (int i = 0; i < event_count && i < EVENT_MAX; i++) {
        n += events[i].type == type;
    }
    return n;
}

static const lmesh_event *last_event(lmesh_event_type type)
{
    for (int i = event_count - 1; i >= 0; i--) {
        if (i < EVENT_MAX && events[i].type == type) {
            return &events[i];
        }
    }
    return NULL;
}

static void test_crc(void)
{
    const uint8_t v[] = "123456789";
    CHECK(lmesh_crc16(v, 9, LMESH_CRC16_INIT) == 0x29B1u);

    clock_ms = 500;
    boot(false);
    lmesh_record *r = put(LMESH_ROLE_RELAY, LMESH_PRIO_NORMAL, 60, 3, 10);
    CHECK(r != NULL && lmesh_record_valid(r));

    static const size_t offsets[] = {
        offsetof(lmesh_record, origin),
        offsetof(lmesh_record, dest),
        offsetof(lmesh_record, id),
        offsetof(lmesh_record, id) + 1,
        offsetof(lmesh_record, type_prio),
        offsetof(lmesh_record, hops),
        offsetof(lmesh_record, lifetime_s),
        offsetof(lmesh_record, lifetime_s) + 3,
        offsetof(lmesh_record, content_len),
        offsetof(lmesh_record, content),
        offsetof(lmesh_record, content) + LMESH_MAX_CONTENT - 1,
        offsetof(lmesh_record, role),
        offsetof(lmesh_record, state),
        offsetof(lmesh_record, attempts),
        offsetof(lmesh_record, result_code),
        offsetof(lmesh_record, in_flight_to),
        offsetof(lmesh_record, acquired_ms),
        offsetof(lmesh_record, next_attempt_ms) + 2,
        offsetof(lmesh_record, e2e_backoff_ms) + 1,
        offsetof(lmesh_record, submitted_ms) + 3,
    };
    uint8_t *bytes = (uint8_t *)r;
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        bytes[offsets[i]] ^= 0x10u;
        CHECK(!lmesh_record_valid(r));
        bytes[offsets[i]] ^= 0x10u;
        CHECK(lmesh_record_valid(r));
    }

    r->content[5] ^= 0x01u;
    CHECK(lmesh_store_find(&node, 3, 10, LMESH_ROLE_MASK(LMESH_ROLE_RELAY)) == NULL);
    r->content[5] ^= 0x01u;
    CHECK(lmesh_store_find(&node, 3, 10, LMESH_ROLE_MASK(LMESH_ROLE_RELAY)) == r);
    CHECK(lmesh_store_find(&node, 3, 10, LMESH_ROLE_MASK(LMESH_ROLE_ORIGIN_COPY)) == NULL);

    r->content[5] ^= 0x01u;
    set_time(1500);
    lmesh_store_tick(&node);
    CHECK(r->role == LMESH_ROLE_FREE);
    CHECK(node.stats.record_crc_errors == 1);
    CHECK(lmesh_store_depth(&node) == 0);
}

static void test_alloc_free_and_history(void)
{
    clock_ms = 2000;
    boot(false);
    lmesh_record *r = lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_ORIGIN_COPY);
    CHECK(r != NULL);
    CHECK(r->role == LMESH_ROLE_ORIGIN_COPY && r->acquired_ms == 2000 && r->lifetime_s == 0 && r->content_len == 0);
    CHECK(lmesh_record_valid(r));
    lmesh_store_release(&node, r);
    CHECK(r->role == LMESH_ROLE_FREE);

    for (uint16_t i = 0; i < LMESH_STORE_SIZE; i++) {
        set_time(2000u + i * 10u);
        CHECK(put(LMESH_ROLE_ORIGIN_COPY, LMESH_PRIO_URGENT, 1000, 7, (uint16_t)(i + 1)) != NULL);
    }
    CHECK(lmesh_store_depth(&node) == LMESH_STORE_SIZE);
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == NULL);

    lmesh_record *a = lmesh_store_find(&node, 7, 5, LMESH_ROLE_MASK(LMESH_ROLE_ORIGIN_COPY));
    lmesh_record *b = lmesh_store_find(&node, 7, 3, LMESH_ROLE_MASK(LMESH_ROLE_ORIGIN_COPY));
    CHECK(a != NULL && b != NULL);
    set_time(5000);
    lmesh_store_retire(&node, a, LMESH_STATE_DELIVERED, LMESH_REASON_RECEIPT);
    set_time(5100);
    lmesh_store_retire(&node, b, LMESH_STATE_DELIVERED, LMESH_REASON_RECEIPT);
    CHECK(a->role == LMESH_ROLE_HISTORY && a->state == LMESH_STATE_DELIVERED && a->acquired_ms == 5000);
    CHECK(lmesh_record_valid(a) && lmesh_record_valid(b));
    CHECK(count_events(LMESH_EV_MSG_STATE) == 2);
    const lmesh_event *ev = last_event(LMESH_EV_MSG_STATE);
    CHECK(ev && ev->u.msg_state.ref.origin == 7 && ev->u.msg_state.ref.id == 3 &&
          ev->u.msg_state.state == LMESH_STATE_DELIVERED && ev->u.msg_state.reason == LMESH_REASON_RECEIPT &&
          ev->u.msg_state.dest == 9 && ev->u.msg_state.prio == LMESH_PRIO_URGENT);
    CHECK(lmesh_store_depth(&node) == LMESH_STORE_SIZE - 2);
    CHECK(lmesh_store_find(&node, 7, 5, LMESH_ROLE_MASK(LMESH_ROLE_HISTORY)) == a);

    lmesh_record *n = lmesh_store_alloc(&node, LMESH_PRIO_ROUTINE, LMESH_ROLE_RELAY);
    CHECK(n == a);
    CHECK(n->role == LMESH_ROLE_RELAY && n->origin == 0 && n->acquired_ms == 5100);
    CHECK(node.stats.evicted == 0);
    n = lmesh_store_alloc(&node, LMESH_PRIO_ROUTINE, LMESH_ROLE_RELAY);
    CHECK(n == b);
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_ROUTINE, LMESH_ROLE_RELAY) == NULL);

    lmesh_store_retire(&node, n, LMESH_STATE_DELIVERED, LMESH_REASON_HOP_ACCEPTED);
    CHECK(n->role == LMESH_ROLE_FREE);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 2);
}

static void test_eviction_order(void)
{
    clock_ms = 10000;
    boot(false);
    for (int i = 0; i < LMESH_STORE_SIZE - 8; i++) {
        uint8_t role = (i % 2) ? LMESH_ROLE_RELAY : LMESH_ROLE_ORIGIN_COPY;
        CHECK(put(role, LMESH_PRIO_URGENT, 5, 50, (uint16_t)(1000 + i)) != NULL);
    }
    lmesh_record *relay_r_far = put(LMESH_ROLE_RELAY, LMESH_PRIO_ROUTINE, 100, 2, 1);
    lmesh_record *relay_r_near = put(LMESH_ROLE_RELAY, LMESH_PRIO_ROUTINE, 50, 2, 2);
    lmesh_record *relay_n = put(LMESH_ROLE_RELAY, LMESH_PRIO_NORMAL, 30, 2, 3);
    lmesh_record *shadow_n_far = put(LMESH_ROLE_ORIGIN_SHADOW, LMESH_PRIO_NORMAL, 45, 7, 4);
    lmesh_record *shadow_n = put(LMESH_ROLE_ORIGIN_SHADOW, LMESH_PRIO_NORMAL, 40, 7, 5);
    lmesh_record *shadow_u = put(LMESH_ROLE_ORIGIN_SHADOW, LMESH_PRIO_URGENT, 10, 7, 6);
    lmesh_record *copy_r = put(LMESH_ROLE_ORIGIN_COPY, LMESH_PRIO_ROUTINE, 70, 7, 7);
    lmesh_record *copy_n = put(LMESH_ROLE_ORIGIN_COPY, LMESH_PRIO_NORMAL, 20, 7, 8);
    CHECK(lmesh_store_depth(&node) == LMESH_STORE_SIZE);
    event_count = 0;

    lmesh_record *got = lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY);
    CHECK(got == relay_r_near);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 0);
    CHECK(count_events(LMESH_EV_STORE) == 1);
    CHECK(node.stats.evicted == 1 && node.store.evicted == 1);
    set_prio(got, LMESH_PRIO_URGENT);

    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == relay_r_far);
    set_prio(relay_r_far, LMESH_PRIO_URGENT);
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == relay_n);
    set_prio(relay_n, LMESH_PRIO_URGENT);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 0);
    CHECK(node.stats.evicted == 3);

    event_count = 0;
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == shadow_n);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 1);
    const lmesh_event *ev = last_event(LMESH_EV_MSG_STATE);
    CHECK(ev && ev->u.msg_state.ref.origin == 7 && ev->u.msg_state.ref.id == 5 &&
          ev->u.msg_state.state == LMESH_STATE_FAILED && ev->u.msg_state.reason == LMESH_REASON_EVICTED);
    CHECK(shadow_n->role == LMESH_ROLE_RELAY && shadow_n->id == 0);
    CHECK(count_role(LMESH_ROLE_HISTORY) == 0);
    set_prio(shadow_n, LMESH_PRIO_URGENT);

    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == shadow_n_far);
    set_prio(shadow_n_far, LMESH_PRIO_URGENT);

    event_count = 0;
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == copy_r);
    ev = last_event(LMESH_EV_MSG_STATE);
    CHECK(ev && ev->u.msg_state.ref.id == 7 && ev->u.msg_state.state == LMESH_STATE_FAILED &&
          ev->u.msg_state.reason == LMESH_REASON_EVICTED && ev->u.msg_state.prio == LMESH_PRIO_ROUTINE);
    set_prio(copy_r, LMESH_PRIO_URGENT);
    CHECK(node.stats.evicted == 6 && node.store.evicted == 6);

    event_count = 0;
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == NULL);
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_ROUTINE, LMESH_ROLE_RELAY) == NULL);
    CHECK(event_count == 0);
    CHECK(shadow_u->role == LMESH_ROLE_ORIGIN_SHADOW && copy_n->role == LMESH_ROLE_ORIGIN_COPY);

    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_URGENT, LMESH_ROLE_RELAY) == shadow_u);
    set_prio(shadow_u, LMESH_PRIO_URGENT);
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_URGENT, LMESH_ROLE_RELAY) == copy_n);
    set_prio(copy_n, LMESH_PRIO_URGENT);
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_URGENT, LMESH_ROLE_RELAY) == NULL);
    CHECK(node.stats.evicted == 8 && node.store.evicted == 8);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 2);
}

static void test_urgent_protected(void)
{
    clock_ms = 0;
    boot(false);
    static const uint8_t roles[] = {LMESH_ROLE_RELAY, LMESH_ROLE_ORIGIN_SHADOW, LMESH_ROLE_ORIGIN_COPY};
    for (int i = 0; i < LMESH_STORE_SIZE; i++) {
        CHECK(put(roles[i % 3], LMESH_PRIO_URGENT, 1, 3, (uint16_t)i) != NULL);
    }
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_NORMAL, LMESH_ROLE_RELAY) == NULL);
    CHECK(lmesh_store_alloc(&node, LMESH_PRIO_ROUTINE, LMESH_ROLE_ORIGIN_COPY) == NULL);
    CHECK(node.stats.evicted == 0 && event_count == 0);
    CHECK(lmesh_store_depth(&node) == LMESH_STORE_SIZE);
    lmesh_record *r = lmesh_store_alloc(&node, LMESH_PRIO_URGENT, LMESH_ROLE_RELAY);
    CHECK(r != NULL && node.stats.evicted == 1);
}

static void test_remaining(void)
{
    clock_ms = 1000;
    boot(false);
    lmesh_record *r = put(LMESH_ROLE_RELAY, LMESH_PRIO_NORMAL, 10, 2, 1);
    set_time(1999);
    CHECK(lmesh_record_remaining_s(&node, r) == 10);
    set_time(2000);
    CHECK(lmesh_record_remaining_s(&node, r) == 9);
    set_time(10999);
    CHECK(lmesh_record_remaining_s(&node, r) == 1);
    set_time(11000);
    CHECK(lmesh_record_remaining_s(&node, r) == 0);
    set_time(500000);
    CHECK(lmesh_record_remaining_s(&node, r) == 0);

    r->acquired_ms = 0xFFFFFC18u;
    lmesh_record_seal(r);
    set_time(1000);
    CHECK(lmesh_record_remaining_s(&node, r) == 8);

    r->lifetime_s = 0xFFFFFFFFu;
    r->acquired_ms = 1000;
    lmesh_record_seal(r);
    node.store.last_sweep_ms = 0;
    set_time(1000u + 0x40000000u + 700u);
    uint32_t before = lmesh_record_remaining_s(&node, r);
    lmesh_store_tick(&node);
    CHECK(r->role == LMESH_ROLE_RELAY && lmesh_record_valid(r));
    CHECK(lmesh_record_remaining_s(&node, r) == before);
    CHECK(r->lifetime_s == before && node.now_ms - r->acquired_ms < 1000u);
    set_time(node.now_ms + 1000000000u);
    CHECK(lmesh_record_remaining_s(&node, r) == before - 1000000u);
}

static void test_expiry(void)
{
    clock_ms = 1000;
    boot(false);
    lmesh_record *origin = put(LMESH_ROLE_ORIGIN_COPY, LMESH_PRIO_NORMAL, 2, 7, 40);
    lmesh_record *relay = put(LMESH_ROLE_RELAY, LMESH_PRIO_ROUTINE, 3, 4, 41);
    lmesh_record *keep = put(LMESH_ROLE_RELAY, LMESH_PRIO_ROUTINE, 100, 4, 42);

    set_time(1500);
    node.next_wake_ms = 99999;
    lmesh_store_tick(&node);
    CHECK(node.store.last_sweep_ms == 1000);
    CHECK(node.next_wake_ms == 2000);

    for (uint32_t t = 2000; t < 3000; t += 250) {
        set_time(t);
        lmesh_store_tick(&node);
    }
    CHECK(origin->role == LMESH_ROLE_ORIGIN_COPY && node.stats.expired == 0);

    set_time(3000);
    node.next_wake_ms = 99999;
    lmesh_store_tick(&node);
    CHECK(node.next_wake_ms == 4000);
    CHECK(origin->role == LMESH_ROLE_HISTORY && origin->state == LMESH_STATE_EXPIRED);
    CHECK(origin->acquired_ms == 3000 && lmesh_record_valid(origin));
    const lmesh_event *ev = last_event(LMESH_EV_MSG_STATE);
    CHECK(ev && ev->u.msg_state.ref.id == 40 && ev->u.msg_state.state == LMESH_STATE_EXPIRED &&
          ev->u.msg_state.reason == LMESH_REASON_LIFETIME);
    CHECK(relay->role == LMESH_ROLE_RELAY);
    CHECK(node.stats.expired == 1 && node.store.expired == 1);
    ev = last_event(LMESH_EV_STORE);
    CHECK(ev && ev->u.store.expired == 1 && ev->u.store.depth == 2);

    set_time(4000);
    lmesh_store_tick(&node);
    CHECK(relay->role == LMESH_ROLE_FREE);
    CHECK(node.stats.expired == 2 && node.store.expired == 2);
    CHECK(count_events(LMESH_EV_MSG_STATE) == 1);
    CHECK(keep->role == LMESH_ROLE_RELAY);
    CHECK(lmesh_store_depth(&node) == 1);

    set_time(3000u + LMESH_HISTORY_HOLD_MS - 1u);
    lmesh_store_tick(&node);
    CHECK(origin->role == LMESH_ROLE_HISTORY);
    set_time(3000u + LMESH_HISTORY_HOLD_MS + 1000u);
    lmesh_store_tick(&node);
    CHECK(origin->role == LMESH_ROLE_FREE);
    CHECK(keep->role == LMESH_ROLE_FREE);
    CHECK(node.stats.expired == 3);
}

static void test_msg_id(void)
{
    wipe_storage();
    clock_ms = 0;
    boot(false);
    CHECK(lmesh_store_next_msg_id(&node) == 1);
    CHECK(lmesh_store_next_msg_id(&node) == 2);
    node.store.next_msg_id = 0xFFFFu;
    CHECK(lmesh_store_next_msg_id(&node) == 0xFFFFu);
    CHECK(lmesh_store_next_msg_id(&node) == 1);
    CHECK(slot_writes[LMESH_SLOT_MSGID] == 0);

    boot(true);
    CHECK(slot_writes[LMESH_SLOT_MSGID] == 1);
    CHECK(lmesh_store_next_msg_id(&node) == 1);
    for (int i = 1; i < LMESH_MSGID_SAVE_EVERY - 1; i++) {
        (void)lmesh_store_next_msg_id(&node);
    }
    CHECK(slot_writes[LMESH_SLOT_MSGID] == 1);
    CHECK(lmesh_store_next_msg_id(&node) == LMESH_MSGID_SAVE_EVERY);
    CHECK(slot_writes[LMESH_SLOT_MSGID] == 2);
    CHECK(lmesh_get16(slot_buf[LMESH_SLOT_MSGID]) == LMESH_MSGID_SAVE_EVERY + 1);
    for (int i = 0; i < 5; i++) {
        (void)lmesh_store_next_msg_id(&node);
    }

    boot(true);
    uint16_t first = lmesh_store_next_msg_id(&node);
    CHECK(first == LMESH_MSGID_SAVE_EVERY + 1 + LMESH_MSGID_BOOT_MARGIN);
    CHECK(first > LMESH_MSGID_SAVE_EVERY + 5);
    for (int i = 0; i < 10; i++) {
        (void)lmesh_store_next_msg_id(&node);
    }
    boot(true);
    CHECK(lmesh_store_next_msg_id(&node) == first + LMESH_MSGID_BOOT_MARGIN);

    slot_buf[LMESH_SLOT_MSGID][0] ^= 0x04u;
    boot(true);
    CHECK(lmesh_store_next_msg_id(&node) == 1);

    uint8_t b[4];
    lmesh_put16(b, (uint16_t)(0x10000 - LMESH_MSGID_BOOT_MARGIN));
    lmesh_put16(b + 2, lmesh_crc16(b, 2, LMESH_CRC16_INIT));
    memcpy(slot_buf[LMESH_SLOT_MSGID], b, 4);
    boot(true);
    CHECK(lmesh_store_next_msg_id(&node) == 1);
}

int main(void)
{
    test_crc();
    test_alloc_free_and_history();
    test_eviction_order();
    test_urgent_protected();
    test_remaining();
    test_expiry();
    test_msg_id();
    printf("%d checks, %d failures: %s\n", checks, failures, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
