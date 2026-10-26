/* Neighbor table: liveness from missed HELLOs and failed unicasts, restart detection, HELLO loss bitmap, bidirectional ETX. */
#include <string.h>

#include "hello.h"
#include "link.h"
#include "message.h"
#include "neighbor.h"
#include "route.h"

_Static_assert(LMESH_ETX_WINDOW >= 2 && LMESH_ETX_WINDOW <= 16, "the HELLO reception bitmap is 16 bits");

#define WINDOW_MASK ((uint32_t)((1ul << LMESH_ETX_WINDOW) - 1u))
#define PESSIMISTIC_BITMAP ((uint16_t)((1u << (LMESH_ETX_WINDOW / 2)) - 1u))
#define DOWN_AFTER_MS \
    ((uint32_t)LMESH_HELLO_MISS_LIMIT * (uint32_t)LMESH_HELLO_PERIOD_MS * (100u + LMESH_HELLO_JITTER_PCT) / 100u)
#define HELLO_SEQ_TOLERANCE 2u
#define ETX8_MIN 8u
#define ETX8_MAX 254u
#define ETX8_BAND_LOW 12u
#define ETX8_BAND_MID 24u

static uint8_t popcount16(uint16_t v)
{
    uint8_t c = 0;
    while (v) {
        v = (uint16_t)(v & (v - 1u));
        c++;
    }
    return c;
}

static uint8_t etx_band(uint8_t etx8)
{
    if (etx8 <= ETX8_BAND_LOW) {
        return 0;
    }
    return etx8 <= ETX8_BAND_MID ? 1u : 2u;
}

static void emit_neighbor(lmesh_node *node, lmesh_event_type type, const lmesh_neighbor *n)
{
    lmesh_event ev = {.type = type, .t_ms = node->now_ms};
    ev.u.neighbor.id = n->id;
    ev.u.neighbor.etx8 = n->etx8;
    ev.u.neighbor.rssi = n->rssi;
    ev.u.neighbor.snr = n->snr;
    lmesh_emit(node, &ev);
}

static void declare_down(lmesh_node *node, lmesh_neighbor *n)
{
    n->up = false;
    n->bidirectional = false;
    n->fail_count = 0;
    n->tx_quality = 0;
    n->etx8 = LMESH_METRIC_INF;
    emit_neighbor(node, LMESH_EV_NEIGHBOR_DOWN, n);
    lmesh_route_neighbor_down(node, n->id);
    lmesh_link_neighbor_reset(node, n->id);
    lmesh_hello_trigger(node);
}

uint8_t lmesh_etx8_compute(uint8_t rx_quality, uint8_t tx_quality, bool tx_reported)
{
    uint32_t num;
    uint32_t den;

    if (rx_quality == 0 || (tx_reported && tx_quality == 0)) {
        return LMESH_METRIC_INF;
    }
    if (tx_reported) {
        num = 8u * 255u * 255u;
        den = (uint32_t)tx_quality * rx_quality;
    } else {
        num = 16u * 255u;
        den = rx_quality;
    }
    uint32_t etx8 = (num + den / 2u) / den;
    if (etx8 < ETX8_MIN) {
        etx8 = ETX8_MIN;
    }
    if (etx8 > ETX8_MAX) {
        etx8 = ETX8_MAX;
    }
    return (uint8_t)etx8;
}

lmesh_neighbor *lmesh_neighbor_find(lmesh_node *node, lmesh_node_id id)
{
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        lmesh_neighbor *n = &node->nbr.table[i];
        if (n->used && n->id == id) {
            return n;
        }
    }
    return NULL;
}

lmesh_neighbor *lmesh_neighbor_touch(lmesh_node *node, lmesh_node_id id, int16_t rssi, int8_t snr)
{
    if (id == LMESH_ID_NONE || id == LMESH_ID_ALL || id == node->ident.id) {
        return NULL;
    }
    lmesh_neighbor *n = lmesh_neighbor_find(node, id);
    if (!n) {
        lmesh_neighbor *victim = NULL;
        uint32_t victim_silence = 0;
        for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
            lmesh_neighbor *c = &node->nbr.table[i];
            if (!c->used) {
                victim = c;
                break;
            }
            uint32_t silence = node->now_ms - c->last_heard_ms;
            if (!c->up && (!victim || silence > victim_silence)) {
                victim = c;
                victim_silence = silence;
            }
        }
        if (!victim) {
            return NULL;
        }
        n = victim;
        memset(n, 0, sizeof *n);
        n->used = true;
        n->id = id;
        n->etx8 = LMESH_METRIC_INF;
    }
    n->last_heard_ms = node->now_ms;
    n->rssi = rssi;
    n->snr = snr;
    return n;
}

void lmesh_neighbor_hello_seen(lmesh_node *node, lmesh_neighbor *n, uint8_t hello_seq, bool listed_us,
                               uint8_t their_rx_quality)
{
    bool was_up = n->up;
    bool was_bidirectional = n->bidirectional;
    uint8_t old_band = etx_band(n->etx8);
    uint8_t gap = (uint8_t)(hello_seq - n->last_hello_seq);

    if (!was_up || gap > LMESH_ETX_WINDOW) {
        n->rx_bitmap = PESSIMISTIC_BITMAP;
    } else if (gap > 0) {
        n->rx_bitmap = (uint16_t)((((uint32_t)n->rx_bitmap << gap) | 1u) & WINDOW_MASK);
    }
    n->last_hello_seq = hello_seq;
    n->last_hello_ms = node->now_ms;
    n->rx_quality = (uint8_t)((uint32_t)popcount16(n->rx_bitmap) * 255u / LMESH_ETX_WINDOW);

    if (listed_us) {
        n->bidirectional = true;
        n->tx_quality = their_rx_quality;
        n->tx_quality_ms = node->now_ms;
    } else {
        n->bidirectional = false;
        n->tx_quality = 0;
    }
    n->etx8 = lmesh_etx8_compute(n->rx_quality, n->tx_quality, n->bidirectional);

    if (!was_up) {
        n->up = true;
        n->fail_count = 0;
        emit_neighbor(node, LMESH_EV_NEIGHBOR_UP, n);
        lmesh_message_route_event(node, n->id);
        lmesh_link_neighbor_reset(node, n->id);
        lmesh_hello_trigger(node);
        return;
    }
    if (was_bidirectional && !n->bidirectional) {
        lmesh_route_neighbor_down(node, n->id);
    }
    if (etx_band(n->etx8) != old_band) {
        emit_neighbor(node, LMESH_EV_LINK_QUALITY, n);
    }
}

bool lmesh_neighbor_restart_check(lmesh_node *node, lmesh_neighbor *n, uint8_t hello_seq, uint16_t own_seqno)
{
    bool seqno_changed = n->own_seqno_valid && own_seqno != n->own_seqno;
    n->own_seqno = own_seqno;
    n->own_seqno_valid = true;
    if (!n->up) {
        return seqno_changed;
    }
    uint8_t gap = (uint8_t)(hello_seq - n->last_hello_seq);
    bool backward = gap >= 0x80u && gap < (uint8_t)(0x100u - HELLO_SEQ_TOLERANCE);
    bool continuous = gap >= 1u && gap <= LMESH_ETX_WINDOW;
    if (backward || (seqno_changed && !continuous)) {
        declare_down(node, n);
        return true;
    }
    return seqno_changed;
}

void lmesh_neighbor_link_result(lmesh_node *node, lmesh_node_id id, bool ok)
{
    lmesh_neighbor *n = lmesh_neighbor_find(node, id);
    if (!n) {
        return;
    }
    if (ok) {
        n->fail_count = 0;
        return;
    }
    if (!n->up) {
        return;
    }
    if (n->fail_count < UINT8_MAX) {
        n->fail_count++;
    }
    if (n->fail_count >= LMESH_LINK_FAIL_LIMIT) {
        declare_down(node, n);
    }
}

void lmesh_neighbor_tick(lmesh_node *node)
{
    for (size_t i = 0; i < LMESH_MAX_NEIGHBORS; i++) {
        lmesh_neighbor *n = &node->nbr.table[i];
        if (!n->used || !n->up) {
            continue;
        }
        uint32_t deadline = n->last_hello_ms + DOWN_AFTER_MS;
        if (lmesh_time_reached(node->now_ms, deadline)) {
            declare_down(node, n);
        } else {
            lmesh_wake_at(node, deadline);
        }
    }
}

uint8_t lmesh_neighbor_etx8(const lmesh_neighbor *n)
{
    if (!n || !n->used || !n->up) {
        return LMESH_METRIC_INF;
    }
    return n->etx8;
}
