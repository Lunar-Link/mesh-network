/* Unit tests for duplicate suppression: sliding windows, older-than-window rule, LRU, result ring, relay cache, persistence. */
#include <stdio.h>
#include <string.h>

#include "dedup.h"
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

static uint16_t highest(lmesh_node_id origin)
{
    const lmesh_dedup_window *w = lmesh_dedup_window_for(&node, origin);
    return w ? w->highest : 0;
}

static void test_window(void)
{
    clock_ms = 0;
    boot(false);
    CHECK(LMESH_DEDUP_WINDOW_BITS == 128);
    CHECK(dest(1, 100));
    CHECK(!dest(1, 100));
    CHECK(dest(1, 101));
    CHECK(node.dedup.windows[0].bitmap[0] == 3u);
    CHECK(dest(1, 99));
    CHECK(!dest(1, 99));
    CHECK(node.stats.dedup_outside == 0);

    CHECK(dest(1, 226));
    CHECK(!dest(1, 99));
    CHECK(!dest(1, 100));
    CHECK(!dest(1, 101));
    CHECK(dest(1, 102));
    CHECK(!dest(1, 102));
    CHECK(node.dedup.windows[0].bitmap[3] == 0xF0000000u);
    CHECK(node.stats.dedup_outside == 0);

    CHECK(dest(1, 98));
    CHECK(dest(1, 98));
    CHECK(dest(1, 50));
    CHECK(node.stats.dedup_outside == 3);
    CHECK(highest(1) == 226);
    CHECK(!dest(1, 99));
    CHECK(!dest(1, 226));

    CHECK(dest(1, 266));
    CHECK(!dest(1, 226));
    CHECK(dest(1, 225));
    CHECK(!dest(1, 225));
    CHECK(dest(1, 140));
    CHECK(!dest(1, 140));
    CHECK(dest(1, 139));
    CHECK(!dest(1, 139));
    CHECK(node.stats.dedup_outside == 3);
    CHECK(dest(1, 138));
    CHECK(dest(1, 138));
    CHECK(node.stats.dedup_outside == 5);

    CHECK(dest(1, 394));
    CHECK(node.dedup.windows[0].bitmap[0] == 1u && node.dedup.windows[0].bitmap[1] == 0u);
    CHECK(node.dedup.windows[0].bitmap[2] == 0u && node.dedup.windows[0].bitmap[3] == 0u);
    CHECK(dest(1, 267));
    CHECK(!dest(1, 267));
    CHECK(dest(1, 266));
    CHECK(node.stats.dedup_outside == 6);
    CHECK(dest(1, (uint16_t)(394 + 0x8000)));
    CHECK(node.stats.dedup_outside == 7 && highest(1) == 394);
    CHECK(dest(1, (uint16_t)(394 + 0x7FFF)));
    CHECK(highest(1) == (uint16_t)(394 + 0x7FFF));

    CHECK(dest(2, 65530));
    CHECK(dest(2, 65535));
    CHECK(dest(2, 0));
    CHECK(dest(2, 3));
    CHECK(dest(2, 65534));
    CHECK(!dest(2, 65534));
    CHECK(!dest(2, 65530));
    CHECK(!dest(2, 0));
    uint32_t outside = node.stats.dedup_outside;
    CHECK(dest(2, (uint16_t)(3 - 127)));
    CHECK(!dest(2, (uint16_t)(3 - 127)));
    CHECK(dest(2, (uint16_t)(3 - 128)));
    CHECK(dest(2, (uint16_t)(3 - 128)));
    CHECK(node.stats.dedup_outside == outside + 2);
    CHECK(dest(2, 1));
    CHECK(highest(2) == 3);

    CHECK(dest(3, 5));
    CHECK(dest(3, 37));
    CHECK(!dest(3, 5));
    CHECK(dest(3, 6));
    CHECK(dest(3, 101));
    CHECK(!dest(3, 5));
    CHECK(!dest(3, 6));
    CHECK(!dest(3, 37));
    CHECK(dest(3, 7));
    CHECK(dest(3, 133));
    outside = node.stats.dedup_outside;
    CHECK(dest(3, 5));
    CHECK(!dest(3, 6));
    CHECK(node.stats.dedup_outside == outside + 1);
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
    CHECK(lmesh_dedup_lookup_result(&node, 4, 5, &code) && code == LMESH_RESULT_EXECUTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 6, &code) && code == LMESH_RESULT_REJECTED);
    lmesh_dedup_remember_result(&node, 9, 1, LMESH_RESULT_EXECUTED);
    CHECK(!lmesh_dedup_lookup_result(&node, 9, 1, &code));
    CHECK(lmesh_dedup_lookup_result(&node, 4, 6, NULL));

    lmesh_dedup_remember_result(&node, 4, 7, LMESH_RESULT_UNSUPPORTED);
    lmesh_dedup_remember_result(&node, 4, 8, LMESH_RESULT_EXECUTED);
    CHECK(LMESH_DEDUP_CMD_HISTORY == 4);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 5, &code) && code == LMESH_RESULT_EXECUTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 6, &code) && code == LMESH_RESULT_REJECTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 7, &code) && code == LMESH_RESULT_UNSUPPORTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 8, &code) && code == LMESH_RESULT_EXECUTED);
    lmesh_dedup_remember_result(&node, 4, 9, LMESH_RESULT_REJECTED);
    CHECK(!lmesh_dedup_lookup_result(&node, 4, 5, &code));
    CHECK(lmesh_dedup_lookup_result(&node, 4, 6, &code) && code == LMESH_RESULT_REJECTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 9, &code) && code == LMESH_RESULT_REJECTED);
    lmesh_dedup_remember_result(&node, 4, 7, LMESH_RESULT_EXECUTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 7, &code) && code == LMESH_RESULT_EXECUTED);
    CHECK(lmesh_dedup_lookup_result(&node, 4, 6, &code) && code == LMESH_RESULT_REJECTED);
    CHECK(lmesh_dedup_window_for(&node, 4)->cmd_count == LMESH_DEDUP_CMD_HISTORY);

    CHECK(dest(4, 300));
    CHECK(lmesh_dedup_lookup_result(&node, 4, 9, &code) && code == LMESH_RESULT_REJECTED);
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
    CHECK(slot_len[LMESH_SLOT_DEDUP] == (5u + LMESH_DEDUP_WINDOW_BITS / 8u + 3u * LMESH_DEDUP_CMD_HISTORY) * LMESH_MAX_NODES + 2u);
    CHECK(!node.dedup.dirty);

    set_time(2000);
    CHECK(dest(4, 11));
    CHECK(dest(5, 300));
    CHECK(!dest(5, 300));
    CHECK(dest(5, 200));
    lmesh_dedup_remember_result(&node, 5, 200, LMESH_RESULT_REJECTED);
    for (uint16_t id = 301; id <= 305; id++) {
        CHECK(dest(5, id));
        lmesh_dedup_remember_result(&node, 5, id, (uint8_t)(id == 302 ? LMESH_RESULT_UNSUPPORTED : LMESH_RESULT_EXECUTED));
    }
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
    CHECK(dest(4, (uint16_t)(11 - LMESH_DEDUP_WINDOW_BITS)));
    CHECK(node.stats.dedup_outside == 1);
    CHECK(!node.dedup.dirty);
    size_t image_len = slot_len[LMESH_SLOT_DEDUP];
    memcpy(slot_buf[3], slot_buf[LMESH_SLOT_DEDUP], image_len);

    clock_ms = 50000;
    boot(true);
    uint8_t code = 0xEE;
    CHECK(lmesh_dedup_window_for(&node, 4) != NULL);
    CHECK(!dest(4, 10));
    CHECK(!dest(4, 11));
    CHECK(!dest(5, 300));
    CHECK(!dest(5, 200));
    CHECK(!dest(5, 305));
    CHECK(!lmesh_dedup_lookup_result(&node, 5, 200, &code));
    CHECK(!lmesh_dedup_lookup_result(&node, 5, 301, &code));
    CHECK(lmesh_dedup_lookup_result(&node, 5, 302, &code) && code == LMESH_RESULT_UNSUPPORTED);
    CHECK(lmesh_dedup_lookup_result(&node, 5, 305, &code) && code == LMESH_RESULT_EXECUTED);
    lmesh_dedup_remember_result(&node, 5, 306, LMESH_RESULT_REJECTED);
    CHECK(!lmesh_dedup_lookup_result(&node, 5, 302, &code));
    CHECK(lmesh_dedup_lookup_result(&node, 5, 303, &code) && code == LMESH_RESULT_EXECUTED);
    CHECK(dest(4, 12));
    CHECK(dest(5, 299));
    CHECK(slot_writes[LMESH_SLOT_DEDUP] == 3);

    slot_buf[LMESH_SLOT_DEDUP][1] ^= 0x01u;
    boot(true);
    CHECK(lmesh_dedup_window_for(&node, 4) == NULL);
    CHECK(lmesh_dedup_window_for(&node, 5) == NULL);

    memcpy(slot_buf[LMESH_SLOT_DEDUP], slot_buf[3], image_len);
    slot_len[LMESH_SLOT_DEDUP] = image_len - 1u;
    boot(true);
    CHECK(lmesh_dedup_window_for(&node, 4) == NULL);
    slot_len[LMESH_SLOT_DEDUP] = image_len;
    boot(true);
    CHECK(lmesh_dedup_window_for(&node, 4) != NULL);

    uint8_t *img = slot_buf[LMESH_SLOT_DEDUP];
    const lmesh_dedup_window *w0 = &node.dedup.windows[0];
    size_t ring_off = 3u + LMESH_DEDUP_WINDOW_BITS / 8u;
    CHECK(img[0] == w0->origin && img[ring_off] == w0->cmd_count && img[ring_off + 1u] == w0->cmd_next);
    img[ring_off] = LMESH_DEDUP_CMD_HISTORY + 1u;
    lmesh_put16(img + image_len - 2u, lmesh_crc16(img, image_len - 2u, LMESH_CRC16_INIT));
    boot(true);
    CHECK(lmesh_dedup_window_for(&node, 4) == NULL);

    boot(false);
    CHECK(dest(4, 10));
    lmesh_dedup_remember_result(&node, 4, 10, LMESH_RESULT_EXECUTED);
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
