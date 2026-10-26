/* Skeleton smoke test: initialization contract, tick, wake bound, argument validation. */
#include <stdio.h>
#include <string.h>

#include "lmesh/lmesh.h"

static int failures;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            failures++;                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                     \
    } while (0)

static uint32_t clock_ms;
static int events;

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

static void on_event(void *ctx, const lmesh_event *ev)
{
    (void)ctx;
    (void)ev;
    events++;
}

static lmesh_node node;

int main(void)
{
    lmesh_identity ident = {.id = 7, .network_id = 1};
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    lmesh_callbacks cb = {.on_event = on_event};

    CHECK(lmesh_init(NULL, &ident, &port, &cb) == LMESH_ERR_ARG);
    lmesh_identity bad = ident;
    bad.id = LMESH_ID_ALL;
    CHECK(lmesh_init(&node, &bad, &port, &cb) == LMESH_ERR_ARG);
    lmesh_port noclock = port;
    noclock.now_ms = NULL;
    CHECK(lmesh_init(&node, &ident, &noclock, &cb) == LMESH_ERR_ARG);

    clock_ms = 1000;
    CHECK(lmesh_init(&node, &ident, &port, &cb) == LMESH_OK);
    CHECK(events == 1);
    CHECK(node.ident.id == 7);

    clock_ms = 1500;
    lmesh_tick(&node);
    CHECK(node.now_ms == 1500);
    CHECK(lmesh_next_wake_ms(&node) <= LMESH_TICK_MAX_SLEEP_MS);

    uint8_t text[] = "hello";
    lmesh_msg_ref ref;
    CHECK(lmesh_send(&node, 7, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 60, text, 5, &ref) == LMESH_ERR_ARG);
    CHECK(lmesh_send(&node, 8, LMESH_MSG_RECEIPT, LMESH_PRIO_NORMAL, 60, text, 5, &ref) == LMESH_ERR_ARG);
    uint8_t big[LMESH_MAX_CONTENT + 1];
    memset(big, 'x', sizeof big);
    CHECK(lmesh_send(&node, 8, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, 60, big, sizeof big, &ref) == LMESH_ERR_TOO_LONG);

    uint8_t garbage[4] = {1, 2, 3, 4};
    lmesh_radio_rx(&node, garbage, sizeof garbage, -90, 2);
    lmesh_stats st;
    lmesh_get_stats(&node, &st);
    CHECK(st.rx_malformed == 1);

    lmesh_neighbor_info nb[LMESH_MAX_NEIGHBORS];
    CHECK(lmesh_neighbors(&node, nb, LMESH_MAX_NEIGHBORS) == 0);

    char line[128];
    CHECK(lmesh_host_command(&node, "id", line, sizeof line) > 0);

    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
