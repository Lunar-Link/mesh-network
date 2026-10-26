/* Message-layer observation: per-message lifecycle, receipts, handovers, and the custody audit of every store. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sim_check.h"

#define RECEIPT_FIXED_LEN 3u

static const char *const state_names[] = {"queued", "transmitted", "delivered", "expired", "failed"};

static const char *state_name(unsigned s)
{
    return s < sizeof state_names / sizeof state_names[0] ? state_names[s] : "?";
}

static void violate(sim_msgobs *mo, const char *fmt, ...)
{
    mo->violations++;
    if (mo->first[0] != '\0') {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mo->first, sizeof mo->first, fmt, ap);
    va_end(ap);
}

static void audit_note(sim_audit *a, const char *fmt, ...)
{
    a->unexplained++;
    if (a->first[0] != '\0') {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->first, sizeof a->first, fmt, ap);
    va_end(ap);
}

static bool same_ref(lmesh_msg_ref a, lmesh_msg_ref b)
{
    return a.origin == b.origin && a.id == b.id;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

bool sim_parse_data(const uint8_t *frame, size_t len, sim_data_view *v)
{
    if (len < LMESH_HEADER_LEN + LMESH_MSG_HEADER_LEN + LMESH_TAG_LEN || (frame[0] & 0x0Fu) != SIM_FT_DATA) {
        return false;
    }
    size_t plen = frame[6];
    if (plen < LMESH_MSG_HEADER_LEN || LMESH_HEADER_LEN + plen + LMESH_TAG_LEN > len) {
        return false;
    }
    const uint8_t *p = frame + LMESH_HEADER_LEN;
    v->tx = frame[2];
    v->rx = frame[3];
    v->seq = frame[4];
    v->retry = (frame[5] & SIM_FLAG_RETRY) != 0;
    v->ref.origin = p[0];
    v->ref.id = get16(p + 1);
    v->dest = p[3];
    v->type = (uint8_t)(p[4] & SIM_MSG_TYPE_MASK);
    v->prio = (uint8_t)((p[4] & SIM_MSG_PRIO_MASK) >> SIM_MSG_PRIO_SHIFT);
    v->resend = (p[4] & SIM_MSG_RESEND) != 0;
    v->lifetime_s = (uint32_t)p[5] | ((uint32_t)p[6] << 8) | ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 24);
    v->hops = p[9];
    v->content = p + LMESH_MSG_HEADER_LEN;
    v->content_len = (uint8_t)(plen - LMESH_MSG_HEADER_LEN);
    return true;
}

sim_track *msgobs_find(sim_msgobs *mo, lmesh_msg_ref ref)
{
    for (uint32_t k = mo->count; k-- > 0;) {
        if (same_ref(mo->t[k].ref, ref)) {
            return &mo->t[k];
        }
    }
    return NULL;
}

bool msgobs_all_terminal(const sim_msgobs *mo)
{
    for (uint32_t k = 0; k < mo->count; k++) {
        if (mo->t[k].terminal == 0) {
            return false;
        }
    }
    return mo->overflow == 0;
}

uint32_t msgobs_count_state(const sim_msgobs *mo, lmesh_msg_state state)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < mo->count; k++) {
        n += mo->t[k].terminal && mo->t[k].final_state == (uint8_t)state ? 1u : 0u;
    }
    return n;
}

static bool reason_fits(uint8_t state, uint8_t reason, bool first)
{
    switch (state) {
    case LMESH_STATE_QUEUED:
        return first ? reason == LMESH_REASON_NONE : reason == LMESH_REASON_RESEND;
    case LMESH_STATE_TRANSMITTED:
        return reason == LMESH_REASON_HOP_ACCEPTED;
    case LMESH_STATE_DELIVERED:
        return reason == LMESH_REASON_RECEIPT;
    case LMESH_STATE_EXPIRED:
        return reason == LMESH_REASON_LIFETIME;
    case LMESH_STATE_FAILED:
        return reason == LMESH_REASON_EVICTED || reason == LMESH_REASON_INVALID || reason == LMESH_REASON_REJECTED ||
               reason == LMESH_REASON_CANCELED;
    default:
        return false;
    }
}

static void state_event(sim_msgobs *mo, const sim_net *net, int node, const lmesh_event *ev)
{
    lmesh_node_id self = net->nodes[node].ident.id;
    lmesh_msg_ref ref = ev->u.msg_state.ref;
    uint8_t state = (uint8_t)ev->u.msg_state.state;
    uint8_t reason = ev->u.msg_state.reason;

    if (ref.origin != self) {
        violate(mo, "node %u raised a state event for %u/%u, which it did not originate", self, ref.origin, ref.id);
        return;
    }
    sim_track *t = msgobs_find(mo, ref);
    bool first = t == NULL;
    if (!t) {
        if (state != LMESH_STATE_QUEUED || reason != LMESH_REASON_NONE) {
            violate(mo, "first event for %u/%u is %s reason %u", ref.origin, ref.id, state_name(state), reason);
            return;
        }
        if (mo->count >= SIM_TRACK_MAX) {
            mo->overflow++;
            return;
        }
        t = &mo->t[mo->count++];
        memset(t, 0, sizeof *t);
        t->ref = ref;
        t->origin = node;
        t->dest = ev->u.msg_state.dest;
        t->type = (uint8_t)ev->u.msg_state.msg_type;
        t->prio = (uint8_t)ev->u.msg_state.prio;
        t->submit_ms = ev->t_ms;
    }
    if (t->terminal) {
        violate(mo, "%u/%u: %s at t=%lu ms after terminal %s", ref.origin, ref.id, state_name(state),
                (unsigned long)ev->t_ms, state_name(t->final_state));
    }
    if (!reason_fits(state, reason, first)) {
        violate(mo, "%u/%u: %s with reason %u at t=%lu ms", ref.origin, ref.id, state_name(state), reason,
                (unsigned long)ev->t_ms);
    }
    if (t->n_states < SIM_TRACK_STATES) {
        t->states[t->n_states] = state;
        t->reasons[t->n_states] = reason;
        t->n_states++;
    }
    if (state == LMESH_STATE_DELIVERED || state == LMESH_STATE_EXPIRED || state == LMESH_STATE_FAILED) {
        t->terminal++;
        t->final_state = state;
        t->final_reason = reason;
        t->terminal_ms = ev->t_ms;
        if (reason == LMESH_REASON_LIFETIME || reason == LMESH_REASON_EVICTED) {
            mo->audit.origin_counted[node]++;
        }
        if (t->terminal > 1) {
            violate(mo, "%u/%u: %u terminal events", ref.origin, ref.id, t->terminal);
        }
    }
    if (state == LMESH_STATE_DELIVERED) {
        t->delivered++;
        t->delivered_ms = ev->t_ms;
        t->result_code = ev->u.msg_state.result_code;
        if (t->delivered > 1) {
            violate(mo, "%u/%u: %u Delivered events", ref.origin, ref.id, t->delivered);
        }
    }
}

void msgobs_event(sim_msgobs *mo, const sim_net *net, int node, const lmesh_event *ev)
{
    if (ev->type == LMESH_EV_STORE) {
        mo->audit.store_events[node]++;
        mo->audit.store_ev_evicted[node] = ev->u.store.evicted;
        mo->audit.store_ev_expired[node] = ev->u.store.expired;
        if (ev->u.store.depth > mo->audit.depth_max[node]) {
            mo->audit.depth_max[node] = ev->u.store.depth;
        }
        return;
    }
    if (ev->type == LMESH_EV_MSG_STATE) {
        state_event(mo, net, node, ev);
        return;
    }
    if (ev->type != LMESH_EV_MSG_RECEIVED) {
        return;
    }
    lmesh_node_id self = net->nodes[node].ident.id;
    lmesh_msg_ref ref = ev->u.msg_received.ref;
    sim_track *t = msgobs_find(mo, ref);
    if (ev->u.msg_received.hops > (unsigned)net->count) {
        violate(mo, "%u/%u delivered at %u with hop count %u", ref.origin, ref.id, self, ev->u.msg_received.hops);
    }
    if (!t) {
        if (mo->overflow == 0) {
            violate(mo, "node %u received %u/%u, which was never submitted", self, ref.origin, ref.id);
        }
        return;
    }
    if (t->dest != self) {
        violate(mo, "%u/%u for %u delivered at %u", ref.origin, ref.id, t->dest, self);
    }
    if (t->received == 0) {
        t->received_ms = ev->t_ms;
        t->rx_hops = ev->u.msg_received.hops;
    }
    if (t->received < UINT8_MAX) {
        t->received++;
    }
}

static void note_receipt(sim_msgobs *mo, const sim_data_view *v, uint32_t now_ms)
{
    for (uint32_t k = 0; k < mo->receipt_count; k++) {
        if (same_ref(mo->receipts[k].ref, v->ref)) {
            return;
        }
    }
    if (mo->receipt_count >= SIM_RECEIPT_MAX || v->content_len < RECEIPT_FIXED_LEN) {
        return;
    }
    sim_receipt *r = &mo->receipts[mo->receipt_count++];
    memset(r, 0, sizeof *r);
    r->ref = v->ref;
    r->dest = v->dest;
    r->orig_id = get16(v->content);
    r->code = v->content[2];
    r->len = v->content_len;
    size_t extra = v->content_len - RECEIPT_FIXED_LEN;
    memcpy(r->data, v->content + RECEIPT_FIXED_LEN, extra < SIM_RECEIPT_DATA ? extra : SIM_RECEIPT_DATA);
    r->first_ms = now_ms;
}

static void add_mark(sim_audit *a, int node, lmesh_msg_ref ref, uint8_t status)
{
    sim_mark *m = &a->marks[node][a->mark_next[node]];
    a->mark_next[node] = (uint8_t)((a->mark_next[node] + 1u) % SIM_MARKS);
    m->used = true;
    m->ref = ref;
    m->status = status;
}

static int take_mark(sim_audit *a, int node, lmesh_msg_ref ref)
{
    for (int k = 0; k < SIM_MARKS; k++) {
        sim_mark *m = &a->marks[node][k];
        if (m->used && same_ref(m->ref, ref)) {
            m->used = false;
            return m->status;
        }
    }
    return -1;
}

void msgobs_frame(sim_msgobs *mo, const sim_net *net, int from, int to, const uint8_t *frame, size_t len)
{
    if (len < LMESH_HEADER_LEN) {
        return;
    }
    sim_audit *a = &mo->audit;
    sim_data_view v;
    bool data = sim_parse_data(frame, len, &v);
    if (to == SIM_TO_AIR) {
        sim_txmap *m = &a->txmap[from][frame[4]];
        m->data = data;
        if (!data) {
            return;
        }
        m->ref = v.ref;
        m->to = v.rx;
        if (v.retry) {
            mo->data_retries++;
        } else {
            mo->data_first++;
            mo->handovers[from]++;
        }
        if (v.resend) {
            mo->resend_frames++;
        }
        if (v.type == LMESH_MSG_RECEIPT) {
            note_receipt(mo, &v, net->now_ms);
            return;
        }
        sim_track *t = msgobs_find(mo, v.ref);
        if (t && t->origin == from) {
            if (v.resend) {
                t->resend_frames++;
            }
            if (!t->tx_seen && !v.retry) {
                t->tx_seen = true;
                t->first_tx_ms = net->now_ms;
                t->handover_index = mo->handovers[from];
            }
        }
        return;
    }
    if ((frame[0] & 0x0Fu) == SIM_FT_HOP_ACK && len >= LMESH_HEADER_LEN + 2u && frame[3] == net->nodes[to].ident.id) {
        const sim_txmap *m = &a->txmap[to][frame[LMESH_HEADER_LEN]];
        uint8_t status = frame[LMESH_HEADER_LEN + 1];
        if (m->data && m->to == frame[2] &&
            (status == SIM_ACK_ACCEPTED || status == SIM_ACK_DUPLICATE || status == SIM_ACK_REFUSED_INVALID)) {
            add_mark(a, to, m->ref, status);
        }
    }
}

static bool is_active(uint8_t role)
{
    return role == SIM_ROLE_ORIGIN_COPY || role == SIM_ROLE_ORIGIN_SHADOW || role == SIM_ROLE_RELAY;
}

static bool held_has(const sim_held *h, size_t n, lmesh_msg_ref ref)
{
    for (size_t k = 0; k < n; k++) {
        if (same_ref(h[k].ref, ref)) {
            return true;
        }
    }
    return false;
}

static void explain(sim_msgobs *mo, const sim_net *net, int i, const sim_held *h, int32_t *budget)
{
    sim_audit *a = &mo->audit;
    lmesh_node_id self = net->nodes[i].ident.id;
    if (h->role != SIM_ROLE_RELAY) {
        const sim_track *t = msgobs_find(mo, h->ref);
        if (t && t->terminal) {
            a->origin_terminal++;
        } else {
            audit_note(a, "t=%lu ms node %u dropped origin record %u/%u with no terminal event",
                       (unsigned long)net->now_ms, self, h->ref.origin, h->ref.id);
        }
        return;
    }
    int status = take_mark(a, i, h->ref);
    if (status == (int)SIM_ACK_ACCEPTED || status == (int)SIM_ACK_DUPLICATE) {
        a->custody++;
    } else if (status == (int)SIM_ACK_REFUSED_INVALID) {
        a->refused++;
    } else if (*budget > 0) {
        (*budget)--;
        a->by_counter++;
    } else {
        audit_note(a, "t=%lu ms node %u dropped relay record %u/%u without custody, expiry or eviction",
                   (unsigned long)net->now_ms, self, h->ref.origin, h->ref.id);
    }
}

void msgobs_sample(sim_msgobs *mo, const sim_net *net)
{
    sim_audit *a = &mo->audit;
    a->samples++;
    for (int i = 0; i < net->count; i++) {
        if (!net->nodes[i].alive) {
            if (a->primed[i]) {
                a->death_lost += a->held_n[i];
            }
            a->primed[i] = false;
            a->held_n[i] = 0;
            a->depth_final[i] = 0;
            memset(a->marks[i], 0, sizeof a->marks[i]);
            continue;
        }
        lmesh_msg_info rows[LMESH_STORE_SIZE + 8];
        size_t n = lmesh_messages(&net->nodes[i].mesh, rows, sizeof rows / sizeof rows[0]);
        if (n > a->listed_max) {
            a->listed_max = n;
        }
        sim_held now[LMESH_STORE_SIZE];
        size_t held = 0;
        for (size_t k = 0; k < n && held < LMESH_STORE_SIZE; k++) {
            if (is_active(rows[k].role)) {
                now[held].ref = rows[k].ref;
                now[held].role = rows[k].role;
                held++;
            }
        }
        a->depth_final[i] = (uint8_t)held;
        if (held > a->depth_max[i]) {
            a->depth_max[i] = (uint8_t)held;
        }
        lmesh_stats st;
        lmesh_get_stats(&net->nodes[i].mesh, &st);
        if (a->primed[i]) {
            int32_t budget = (int32_t)(st.expired - a->stat_expired[i]) + (int32_t)(st.evicted - a->stat_evicted[i]) -
                             (int32_t)a->origin_counted[i];
            for (size_t k = 0; k < a->held_n[i]; k++) {
                if (!held_has(now, held, a->held[i][k].ref)) {
                    explain(mo, net, i, &a->held[i][k], &budget);
                }
            }
        }
        for (int k = 0; k < SIM_MARKS; k++) {
            if (a->marks[i][k].used && held_has(now, held, a->marks[i][k].ref)) {
                a->marks[i][k].used = false;
            }
        }
        memcpy(a->held[i], now, held * sizeof now[0]);
        a->held_n[i] = (uint8_t)held;
        a->stat_expired[i] = st.expired;
        a->stat_evicted[i] = st.evicted;
        a->origin_counted[i] = 0;
        a->primed[i] = true;
    }
}

static int cmp_u32(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x;
    uint32_t b = *(const uint32_t *)y;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double percentile(const uint32_t *v, uint32_t n, unsigned pct)
{
    uint32_t rank = (n * pct + 99u) / 100u;
    return v[rank > 0 ? rank - 1u : 0u];
}

static void latency_set(sim_metrics *m, uint32_t *v, uint32_t n, sim_metric_id mean, sim_metric_id p50,
                        sim_metric_id p95, sim_metric_id max)
{
    if (n == 0) {
        return;
    }
    double sum = 0;
    for (uint32_t k = 0; k < n; k++) {
        sum += v[k];
    }
    qsort(v, n, sizeof v[0], cmp_u32);
    metric_set(m, mean, sum / n);
    metric_set(m, p50, percentile(v, n, 50));
    metric_set(m, p95, percentile(v, n, 95));
    metric_set(m, max, v[n - 1]);
}

static void by_node(char *out, size_t out_max, const uint8_t *v, int n)
{
    size_t pos = 0;
    out[0] = '\0';
    for (int i = 0; i < n && pos + 5 < out_max; i++) {
        int w = snprintf(out + pos, out_max - pos, "%s%u", i ? ";" : "", v[i]);
        pos += w > 0 ? (size_t)w : 0u;
    }
}

void msgobs_metrics(const sim_msgobs *mo, const sim_net *net, sim_metrics *m)
{
    static uint32_t lat[SIM_TRACK_MAX];
    static uint32_t rx[SIM_TRACK_MAX];
    uint32_t n_lat = 0, n_rx = 0, received = 0, open = 0, ghost = 0, multi = 0;
    const sim_audit *a = &mo->audit;

    for (uint32_t k = 0; k < mo->count; k++) {
        const sim_track *t = &mo->t[k];
        open += t->terminal ? 0u : 1u;
        if (t->received) {
            received++;
            rx[n_rx++] = t->received_ms - t->submit_ms;
        }
        multi += t->received > 1 ? 1u : 0u;
        if (t->delivered) {
            lat[n_lat++] = t->delivered_ms - t->submit_ms;
            if (!t->received) {
                ghost++;
            }
        }
    }
    metric_set(m, M_MSGS_ORIGINATED, mo->count);
    metric_set(m, M_MSGS_RECEIVED, received);
    metric_set(m, M_MSGS_DELIVERED, msgobs_count_state(mo, LMESH_STATE_DELIVERED));
    metric_set(m, M_MSGS_EXPIRED, msgobs_count_state(mo, LMESH_STATE_EXPIRED));
    metric_set(m, M_MSGS_FAILED, msgobs_count_state(mo, LMESH_STATE_FAILED));
    metric_set(m, M_MSGS_OPEN, open);
    if (mo->count > 0) {
        metric_set(m, M_DELIVERY_RATIO, (double)received / mo->count);
    }
    latency_set(m, lat, n_lat, M_LATENCY_MEAN_MS, M_LATENCY_P50_MS, M_LATENCY_P95_MS, M_LATENCY_MAX_MS);
    latency_set(m, rx, n_rx, M_RX_LATENCY_MEAN_MS, M_RX_LATENCY_P50_MS, M_RX_LATENCY_P95_MS, M_RX_LATENCY_MAX_MS);

    uint8_t depth_max = 0, depth_final = 0;
    for (int i = 0; i < net->count; i++) {
        depth_max = a->depth_max[i] > depth_max ? a->depth_max[i] : depth_max;
        depth_final = a->depth_final[i] > depth_final ? a->depth_final[i] : depth_final;
    }
    metric_set(m, M_STORE_DEPTH_MAX, depth_max);
    metric_set(m, M_STORE_DEPTH_FINAL, depth_final);
    by_node(m->depth_max_by_node, sizeof m->depth_max_by_node, a->depth_max, net->count);
    by_node(m->depth_final_by_node, sizeof m->depth_final_by_node, a->depth_final, net->count);

    uint32_t evicted = 0, expired = 0;
    for (int i = 0; i < net->count; i++) {
        lmesh_stats st;
        lmesh_get_stats(&net->nodes[i].mesh, &st);
        evicted += st.evicted;
        expired += st.expired;
    }
    metric_set(m, M_EVICTIONS, evicted);
    metric_set(m, M_EXPIRIES, expired);
    metric_set(m, M_RECEIPTS_SENT, mo->receipt_count);
    metric_set(m, M_DATA_FIRST_TX, mo->data_first);
    metric_set(m, M_DATA_RETRIES, mo->data_retries);
    metric_set(m, M_RESEND_FRAMES, mo->resend_frames);
    metric_set(m, M_TERMINAL_VIOLATIONS, mo->violations + ghost + multi);
    metric_set(m, M_STORE_CUSTODY, a->custody);
    metric_set(m, M_STORE_BY_COUNTER, a->by_counter);
    metric_set(m, M_STORE_ORIGIN_TERMINAL, a->origin_terminal);
    metric_set(m, M_STORE_REFUSED, a->refused);
    metric_set(m, M_STORE_DEATH_LOST, a->death_lost);
    metric_set(m, M_STORE_UNEXPLAINED, a->unexplained);
    metric_set(m, M_STORE_LISTED_MAX, (double)a->listed_max);

    if (mo->violations > 0) {
        metrics_fail(m, "%lu message-state violations, first: %s", (unsigned long)mo->violations, mo->first);
    }
    if (ghost > 0) {
        metrics_fail(m, "%lu messages Delivered at the origin but never delivered at the destination",
                     (unsigned long)ghost);
    }
    if (mo->overflow > 0) {
        metrics_fail(m, "message tracker overflow (%lu)", (unsigned long)mo->overflow);
    }
    if (a->unexplained > 0) {
        metrics_fail(m, "%lu stored records left without custody, expiry or eviction, first: %s",
                     (unsigned long)a->unexplained, a->first);
    }
    if (a->listed_max > LMESH_STORE_SIZE) {
        metrics_fail(m, "lmesh_messages listed %lu records, store size %d", (unsigned long)a->listed_max,
                     LMESH_STORE_SIZE);
    }
}

static void msgs_path(const char *csv, char *out, size_t out_max)
{
    size_t n = strlen(csv);
    if (n > 4 && strcmp(csv + n - 4, ".csv") == 0) {
        snprintf(out, out_max, "%.*s_msgs.csv", (int)(n - 4), csv);
    } else {
        snprintf(out, out_max, "%s_msgs.csv", csv);
    }
}

int msgobs_csv_append(const sim_msgobs *mo, const sim_metrics *m, const char *csv)
{
    if (mo->count == 0) {
        return 0;
    }
    char path[512];
    msgs_path(csv, path, sizeof path);
    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
        fprintf(f, "scenario,seed,origin,id,dest,type,prio,submit_ms,first_tx_ms,handover,final_state,final_reason,"
                   "result_code,received,rx_hops,resend_frames,latency_ms,rx_latency_ms\n");
    }
    for (uint32_t k = 0; k < mo->count; k++) {
        const sim_track *t = &mo->t[k];
        fprintf(f, "%s,%lu,%u,%u,%u,%u,%u,%lu,", m->scenario, (unsigned long)m->seed, t->ref.origin, t->ref.id,
                t->dest, t->type, t->prio, (unsigned long)t->submit_ms);
        if (t->tx_seen) {
            fprintf(f, "%lu,%lu,", (unsigned long)t->first_tx_ms, (unsigned long)t->handover_index);
        } else {
            fprintf(f, ",,");
        }
        fprintf(f, "%s,%u,%u,%u,%u,%u,", t->terminal ? state_name(t->final_state) : "open", t->final_reason,
                t->result_code, t->received, t->rx_hops, t->resend_frames);
        if (t->delivered) {
            fprintf(f, "%lu,", (unsigned long)(t->delivered_ms - t->submit_ms));
        } else {
            fprintf(f, ",");
        }
        if (t->received) {
            fprintf(f, "%lu\n", (unsigned long)(t->received_ms - t->submit_ms));
        } else {
            fprintf(f, "\n");
        }
    }
    fclose(f);
    return 0;
}
