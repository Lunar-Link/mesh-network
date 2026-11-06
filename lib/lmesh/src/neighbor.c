/* Neighbor table: phi accrual liveness from HELLO arrival times, failed unicasts, restart detection, HELLO loss bitmap,
 * bidirectional ETX. */
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

/* Phi accrual (Hayashibara et al., 2004). phi = -log10(P_later(t)), where P_later is the probability that the next
 * periodic HELLO arrives later than t after the last one heard, under a normal fit to the recent intervals between
 * periodic HELLOs. The normal tail uses the logistic approximation 1 / (1 + exp(z)), z = y (1.5976 + 0.070566 y^2),
 * y = (t - mean) / sd, which gives phi = log10(1 + exp(z)). All arithmetic is integer: y and z in Q10, phi in
 * hundredths. */
#define PHI_Q 10
#define PHI_ONE (1 << PHI_Q)
#define PHI_Y_MAX (16 * PHI_ONE)
#define PHI_A_Q10 1636
#define PHI_B_Q16 4625
#define PHI_LN10_Q10 2358
#define PHI100_MAX 13660u

_Static_assert(LMESH_PHI_WINDOW >= 2 && LMESH_PHI_WINDOW <= 255, "phi window counts fit in a byte");
_Static_assert(LMESH_PHI_THRESHOLD > 30 && LMESH_PHI_THRESHOLD < PHI100_MAX,
               "phi threshold, in hundredths, must lie above phi at the mean and below phi at 16 deviations");
_Static_assert(LMESH_PHI_MIN_SD_MS > 0, "the deviation floor must be positive");

/* 1000 log10(1 + exp(-k/4)), k = 0..32. */
static const uint16_t phi_tail1000[33] = {301, 250, 206, 168, 136, 109, 87, 70, 55, 44, 34, 27, 21, 17, 13, 10, 8,
                                          6,   5,   4,   3,   2,   2,   1,  1,  1,  1,  1,  0,  0,  0,  0,  0};

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

static uint32_t phi_tail(uint32_t u_q10)
{
    uint32_t idx = u_q10 >> (PHI_Q - 2);
    if (idx >= 32u) {
        return 0;
    }
    uint32_t frac = u_q10 & ((1u << (PHI_Q - 2)) - 1u);
    uint32_t span = 1u << (PHI_Q - 2);
    return (phi_tail1000[idx] * (span - frac) + phi_tail1000[idx + 1u] * frac + span / 2u) / span;
}

static uint16_t phi100_of_y(int32_t y_q10)
{
    int64_t y = y_q10;
    int64_t y2 = (y * y) / PHI_ONE;
    int64_t c = PHI_A_Q10 + (y2 * PHI_B_Q16) / 65536;
    int64_t z = (y * c) / PHI_ONE;
    uint32_t p1000;
    if (z >= 0) {
        p1000 = (uint32_t)((z * 1000 + PHI_LN10_Q10 / 2) / PHI_LN10_Q10) + phi_tail((uint32_t)z);
    } else {
        p1000 = phi_tail((uint32_t)-z);
    }
    uint32_t p100 = (p1000 + 5u) / 10u;
    return (uint16_t)(p100 > PHI100_MAX ? PHI100_MAX : p100);
}

static int32_t phi_threshold_y(void)
{
    int32_t lo = 0;
    int32_t hi = PHI_Y_MAX;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (phi100_of_y(mid) >= LMESH_PHI_THRESHOLD) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t root = 0;
    uint64_t bit = 1ull << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit) {
        if (v >= root + bit) {
            v -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

uint16_t lmesh_phi100(uint32_t elapsed_ms, uint32_t mean_ms, uint32_t sd_ms)
{
    if (sd_ms == 0) {
        sd_ms = 1;
    }
    int64_t y = ((int64_t)elapsed_ms - (int64_t)mean_ms) * PHI_ONE / (int64_t)sd_ms;
    if (y > PHI_Y_MAX) {
        y = PHI_Y_MAX;
    } else if (y < -PHI_Y_MAX) {
        y = -PHI_Y_MAX;
    }
    return phi100_of_y((int32_t)y);
}

static uint32_t phi_limit(uint32_t mean_ms, uint32_t sd_ms)
{
    uint64_t margin = ((uint64_t)phi_threshold_y() * sd_ms + PHI_ONE - 1u) / PHI_ONE;
    return mean_ms + (uint32_t)LMESH_PHI_PAUSE_MS + (uint32_t)margin;
}

static void phi_refresh(lmesh_neighbor *n)
{
    uint32_t mean = LMESH_HELLO_PERIOD_MS;
    uint32_t sd = 0;
    if (n->phi_count > 0) {
        uint64_t sum = 0;
        for (uint8_t i = 0; i < n->phi_count; i++) {
            sum += n->phi_interval_ms[i];
        }
        mean = (uint32_t)((sum + n->phi_count / 2u) / n->phi_count);
        uint64_t var = 0;
        for (uint8_t i = 0; i < n->phi_count; i++) {
            int64_t d = (int64_t)n->phi_interval_ms[i] - (int64_t)mean;
            var += (uint64_t)(d * d);
        }
        sd = isqrt64(var / n->phi_count);
    }
    if (sd < LMESH_PHI_MIN_SD_MS) {
        sd = LMESH_PHI_MIN_SD_MS;
    }
    n->phi_mean_ms = mean;
    n->phi_sd_ms = sd;
    n->phi_limit_ms = phi_limit(mean, sd);
}

static void phi_reset(lmesh_neighbor *n)
{
    n->periodic_valid = false;
    n->phi_count = 0;
    n->phi_next = 0;
    phi_refresh(n);
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
        phi_reset(n);
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

void lmesh_neighbor_hello_timing(lmesh_node *node, lmesh_neighbor *n, bool periodic)
{
    if (!n || !n->up || !periodic) {
        return;
    }
    if (n->periodic_valid) {
        n->phi_interval_ms[n->phi_next] = node->now_ms - n->last_periodic_ms;
        n->phi_next = (uint8_t)((n->phi_next + 1u) % LMESH_PHI_WINDOW);
        if (n->phi_count < LMESH_PHI_WINDOW) {
            n->phi_count++;
        }
        phi_refresh(n);
    }
    n->last_periodic_ms = node->now_ms;
    n->periodic_valid = true;
}

uint32_t lmesh_neighbor_silence_limit_ms(const lmesh_neighbor *n)
{
#if LMESH_PHI_ACCRUAL
    if (n->phi_sd_ms == 0) {
        return phi_limit(LMESH_HELLO_PERIOD_MS, LMESH_PHI_MIN_SD_MS);
    }
    return n->phi_limit_ms;
#else
    (void)n;
    return DOWN_AFTER_MS;
#endif
}

uint16_t lmesh_neighbor_phi100(const lmesh_node *node, const lmesh_neighbor *n)
{
    if (!n || !n->used) {
        return 0;
    }
    uint32_t mean = n->phi_sd_ms ? n->phi_mean_ms : (uint32_t)LMESH_HELLO_PERIOD_MS;
    uint32_t sd = n->phi_sd_ms ? n->phi_sd_ms : (uint32_t)LMESH_PHI_MIN_SD_MS;
    return lmesh_phi100(node->now_ms - n->last_hello_ms, mean + (uint32_t)LMESH_PHI_PAUSE_MS, sd);
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
        uint32_t deadline = n->last_hello_ms + lmesh_neighbor_silence_limit_ms(n);
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
