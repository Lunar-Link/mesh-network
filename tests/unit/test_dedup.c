/* Unit tests for duplicate suppression: sliding windows, LRU replacement, result replay, relay cache, persistence. */
#include <stdio.h>
#include <string.h>

#include "dedup.h"

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
#define SLOT_MAX 4096

static uint32_t clock_ms;
static uint8_t slot_buf[SLOTS][SLOT_MAX];
static size_t slot_len[SLOTS];
static int slot_writes[SLOTS];

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

static lmesh_node node;

static void boot(bool storage)
{
    lmesh_identity ident = {.id = 7, .network_id = 1};
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    if (storage) {
        port.storage_read = storage_read;
        port.storage_write = storage_write;
    }
    lmesh_callbacks cb = {0};
    CHECK(lmesh_init(&node, &ident, &port, &cb) == LMESH_OK);
}

static void set_time(uint32_t t)
{
    clock_ms = t;
    node.now_ms = t;
}

static bool dest(lmesh_node_id origin, uint16_t id)
{
    return lmesh_dedup_dest_accept(&node, origin, id);
}

static bool relay(lmesh_node_id origin, uint16_t id)
{
    return lmesh_dedup_relay_accept(&node, origin, id);
}

static void test_window(void)
{
    clock_ms = 0;
    boot(false);
    CHECK(dest(1, 100));
    CHECK(!dest(1, 100));
    CHECK(dest(1, 101));
    CHECK(dest(1, 99));
    CHECK(!dest(1, 99));
    CHECK(dest(1, 131));
    CHECK(!dest(1, 100));
    CHECK(!dest(1, 99));
    CHECK(!dest(1, 98));
    CHECK(!dest(1, 131));
    CHECK(dest(1, 130));
    CHECK(dest(1, 171));
    CHECK(node.dedup.windows[0].bitmap == 1u);
    CHECK(dest(1, 140));
    CHECK(!dest(1, 139));
    CHECK(!dest(1, 131));
    CHECK(!dest(1, (uint16_t)(171 + 0x8000)));
    CHECK(dest(1, (uint16_t)(171 + 0x7FFF)));

    CHECK(dest(2, 65530));
    CHECK(dest(2, 65535));
    CHECK(dest(2, 0));
    CHECK(dest(2, 3));
    CHECK(dest(2, 65534));
    CHECK(!dest(2, 65534));
    CHECK(!dest(2, 65530));
    CHECK(!dest(2, 0));
    CHECK(dest(2, 65508));
    CHECK(!dest(2, 65507));
    CHECK(dest(2, 1));
    CHECK(lmesh_dedup_window_for(&node, 2)->highest == 3);

    CHECK(dest(3, 5));
    CHECK(!dest(3, 5));
    CHECK(lmesh_dedup_window_for(&node, 1) != lmesh_dedup_window_for(&node, 3));
}

static void test_lru(void)
{
    clock_ms = 0;
    boot(false);
    for (int i = 0; i < LMESH_MAX_NODES; i++) {
        set_time(1000u + (uint32_t)i * 10u);
        CHECK(dest((lmesh_node_id)(i + 1), 1));
    }
    set_time(5000);
    CHECK(!dest(1, 1));
    set_time(6000);
    CHECK(dest(100, 1));
    CHECK(lmesh_dedup_window_for(&node, 2) == NULL);
    CHECK(lmesh_dedup_window_for(&node, 1) != NULL);
    CHECK(lmesh_dedup_window_for(&node, 100) != NULL);
    CHECK(!dest(100, 1));
    set_time(7000);
    CHECK(dest(2, 1));
    CHECK(lmesh_dedup_window_for(&node, 3) == NULL);
    CHECK(!dest(1, 1));
}

static void test_results(void)
{
    clock_ms = 0;
    boot(false);
    uint8_t code = 0xEE;
    CHECK(!lmesh_dedup_lookup_result(&node, 4, 0, &code));
    CHECK(dest(4, 0));
    CHECK(!lmesh_dedup_lookup_result(&node, 4, 0, &code));
    CHECK(dest(4, 5));
    lmesh_dedup_remember_result(&node, 4, 5, LMESH_RESULT_EXECUTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 5, &code) && code == LMESH_RESULT_EXECUTED);
    CHECK(!lmesh_dedup_lookup_result(&node, 4, 6, &code));
    CHECK(!lmesh_dedup_lookup_result(&node, 9, 5, &code));
    CHECK(dest(4, 6));
    lmesh_dedup_remember_result(&node, 4, 6, LMESH_RESULT_REJECTED);
    CHECK(!lmesh_dedup_lookup_result(&node, 4, 5, &code));
    CHECK(lmesh_dedup_lookup_result(&node, 4, 6, &code) && code == LMESH_RESULT_REJECTED);
    lmesh_dedup_remember_result(&node, 9, 1, LMESH_RESULT_EXECUTED);
    CHECK(!lmesh_dedup_lookup_result(&node, 9, 1, &code));
}

static int fwd_used(void)
{
    int n = 0;
    for (size_t i = 0; i < LMESH_FWD_CACHE; i++) {
        n += node.dedup.fwd[i].used;
    }
    return n;
}

static void test_relay_cache(void)
{
    clock_ms = 0;
    boot(false);
    CHECK(relay(1, 10));
    CHECK(!relay(1, 10));
    CHECK(relay(1, 11));
    CHECK(relay(2, 10));
    for (int i = 0; i < LMESH_FWD_CACHE; i++) {
        CHECK(relay(9, (uint16_t)(1000 + i)));
    }
    CHECK(!relay(9, 1000));
    CHECK(relay(1, 10));
    CHECK(relay(9, 1000));
    CHECK(!relay(9, 1002));
    CHECK(!relay(9, (uint16_t)(1000 + LMESH_FWD_CACHE - 1)));

    clock_ms = 1000;
    boot(false);
    CHECK(relay(3, 1));
    set_time(1000u + LMESH_FWD_CACHE_HOLD_MS - 1u);
    CHECK(!relay(3, 1));
    lmesh_dedup_tick(&node);
    CHECK(fwd_used() == 1);
    set_time(1000u + LMESH_FWD_CACHE_HOLD_MS);
    lmesh_dedup_tick(&node);
    CHECK(fwd_used() == 0);
    CHECK(relay(3, 1));
    CHECK(!relay(3, 1));

    clock_ms = 0;
    boot(false);
    CHECK(relay(5, 5));
    set_time(LMESH_FWD_CACHE_HOLD_MS);
    CHECK(relay(5, 5));
    CHECK(!relay(5, 5));
}

static void test_persistence(void)
{
    memset(slot_len, 0, sizeof slot_len);
    memset(slot_writes, 0, sizeof slot_writes);
    clock_ms = 1000;
    boot(true);
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 0);
    CHECK(dest(4, 10));
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 1);
    CHECK(!node.dedup.dirty);

    set_time(2000);
    CHECK(dest(4, 11));
    CHECK(dest(5, 300));
    CHECK(!dest(5, 300));
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 1);
    CHECK(node.dedup.dirty);

    set_time(5000);
    node.next_wake_ms = 99999;
    lmesh_dedup_tick(&node);
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 1);
    CHECK(node.next_wake_ms == 1000u + LMESH_DEDUP_SAVE_GAP_MS);

    set_time(1000u + LMESH_DEDUP_SAVE_GAP_MS);
    lmesh_dedup_tick(&node);
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 2);
    CHECK(!node.dedup.dirty);
    lmesh_dedup_tick(&node);
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 2);
    CHECK(!dest(4, 11));
    CHECK(!node.dedup.dirty);

    clock_ms = 50000;
    boot(true);
    CHECK(lmesh_dedup_window_for(&node, 4) != NULL);
    CHECK(!dest(4, 10));
    CHECK(!dest(4, 11));
    CHECK(!dest(5, 300));
    CHECK(dest(4, 12));
    CHECK(dest(5, 299));
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 3);

    slot_buf[LMESH_SLOT_DEDUP][1] ^= 0x01u;
    boot(true);
    CHECK(lmesh_dedup_window_for(&node, 4) == NULL);
    CHECK(lmesh_dedup_window_for(&node, 5) == NULL);

    boot(false);
    CHECK(dest(4, 10));
    CHECK(!node.dedup.dirty);
}

int main(void)
{
    test_window();
    test_lru();
    test_results();
    test_relay_cache();
    test_persistence();
    printf("%d checks, %d failures: %s\n", checks, failures, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
