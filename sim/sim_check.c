/* Simulator invariants and metrics: route-graph walks, convergence tracking, event and frame observation, CSV output. */
#include "sim_check.h"

#include <stdarg.h>
#include <string.h>

#define ETX8_ONE 8
#define METRIC_MAX_FINITE 254u
#define FLOOD_ORIGIN_AT (LMESH_HEADER_LEN)
#define DATA_HOPS_AT (LMESH_HEADER_LEN + 9)
#define MAX_PRINTED_FAILURES 8

static uint32_t hello_heard_ms[SIM_MAX_NODES][SIM_MAX_NODES];

static void obs_frame(void *ctx, int from, int to, const uint8_t *frame, size_t len)
{
    sim_obs *o = (sim_obs *)ctx;
    if (len < LMESH_HEADER_LEN) {
        return;
    }
    unsigned type = frame[0] & 0x0Fu;
    if (to == SIM_TO_AIR) {
        o->frames_tx[type]++;
        if (type == SIM_FT_ROUTE_REQ && len > FLOOD_ORIGIN_AT && frame[FLOOD_ORIGIN_AT] == frame[2]) {
            o->req_originated++;
        }
    } else {
        o->frames_rx[type]++;
        if (type == SIM_FT_HELLO) {
            hello_heard_ms[to][from] = o->net->now_ms;
        }
        if (type == SIM_FT_DATA && len > DATA_HOPS_AT) {
            uint8_t hops = frame[DATA_HOPS_AT];
            if (hops > o->data_hops_max) {
                o->data_hops_max = hops;
            }
            if (hops > (uint8_t)o->net->count) {
                o->hop_violations++;
            }
        }
    }
    msgobs_frame(&o->msg, o->net, from, to, frame, len);
    if (o->user_hook) {
        o->user_hook(o->user_ctx, from, to, frame, len);
    }
}

void obs_init(sim_obs *o, sim_net *net, bool verbose)
{
    memset(o, 0, sizeof *o);
    o->net = net;
    o->verbose = verbose;
    net->on_frame = obs_frame;
    net->frame_ctx = o;
}

bool obs_received(const sim_obs *o, int node, lmesh_msg_ref ref)
{
    for (size_t k = 0; k < o->msg_count[node]; k++) {
        if (o->msgs[node][k].origin == ref.origin && o->msgs[node][k].id == ref.id) {
            return true;
        }
    }
    return false;
}

void obs_event(sim_obs *o, int node, const lmesh_event *ev)
{
    if (node < 0 || node >= SIM_MAX_NODES || (unsigned)ev->type >= SIM_EVENT_TYPES) {
        return;
    }
    o->events[node][ev->type]++;
    msgobs_event(&o->msg, o->net, node, ev);
    if (ev->type == LMESH_EV_ROUTE_LOST || ev->type == LMESH_EV_ROUTE_CHANGED) {
        int d = sim_index_of(o->net, ev->u.route.dest);
        if (d >= 0) {
            if (ev->type == LMESH_EV_ROUTE_LOST) {
                o->route_lost[node][d]++;
            } else {
                o->route_changed[node][d]++;
                o->last_hop[node][d] = ev->u.route.next_hop;
            }
        }
    }
    if (ev->type == LMESH_EV_MSG_RECEIVED) {
        if (obs_received(o, node, ev->u.msg_received.ref)) {
            o->msg_duplicates[node]++;
        } else if (o->msg_count[node] < SIM_MSG_LOG) {
            o->msgs[node][o->msg_count[node]++] = ev->u.msg_received.ref;
        } else {
            o->msg_overflow++;
        }
    }
    if (o->verbose) {
        char line[256];
        size_t n = lmesh_host_format_event(ev, line, sizeof line);
        if (n > 0) {
            printf("node %u %s", o->net->nodes[node].ident.id, line);
        }
    }
}

void obs_clear_route_events(sim_obs *o)
{
    memset(o->route_lost, 0, sizeof o->route_lost);
    memset(o->route_changed, 0, sizeof o->route_changed);
    memset(o->last_hop, 0, sizeof o->last_hop);
}

uint32_t obs_duplicates(const sim_obs *o)
{
    uint32_t d = 0;
    for (int i = 0; i < SIM_MAX_NODES; i++) {
        d += o->msg_duplicates[i];
    }
    return d;
}

typedef struct route_snapshot {
    size_t count[SIM_MAX_NODES];
    lmesh_route_info rows[SIM_MAX_NODES][LMESH_MAX_NODES];
    uint8_t link_etx8[SIM_MAX_NODES][SIM_MAX_NODES];
    uint32_t link_heard_ms[SIM_MAX_NODES][SIM_MAX_NODES];
} route_snapshot;

typedef struct route_history {
    bool seen[SIM_MAX_NODES][SIM_MAX_NODES];
    lmesh_route_info row[SIM_MAX_NODES][SIM_MAX_NODES];
    uint32_t since_ms[SIM_MAX_NODES][SIM_MAX_NODES];
} route_history;

static route_snapshot snap;
static route_history hist;

void check_reset(void)
{
    memset(&snap, 0, sizeof snap);
    memset(&hist, 0, sizeof hist);
    memset(hello_heard_ms, 0, sizeof hello_heard_ms);
}

static void note_history(const sim_net *net, int i)
{
    bool present[SIM_MAX_NODES] = {false};
    for (size_t k = 0; k < snap.count[i]; k++) {
        const lmesh_route_info *r = &snap.rows[i][k];
        int d = sim_index_of(net, r->dest);
        if (d < 0) {
            continue;
        }
        present[d] = true;
        const lmesh_route_info *p = &hist.row[i][d];
        if (!hist.seen[i][d] || p->next_hop != r->next_hop || p->metric8 != r->metric8 || p->seqno != r->seqno ||
            p->retracted != r->retracted) {
            hist.seen[i][d] = true;
            hist.row[i][d] = *r;
            hist.since_ms[i][d] = net->now_ms;
        }
    }
    for (int d = 0; d < net->count; d++) {
        if (!present[d] && hist.seen[i][d]) {
            hist.seen[i][d] = false;
            hist.since_ms[i][d] = net->now_ms;
        }
    }
}

static void take_snapshot(const sim_net *net)
{
    memset(&snap, 0, sizeof snap);
    for (int i = 0; i < net->count; i++) {
        snap.count[i] = lmesh_routes(&net->nodes[i].mesh, snap.rows[i], LMESH_MAX_NODES);
        note_history(net, i);
        lmesh_neighbor_info nb[LMESH_MAX_NEIGHBORS];
        size_t n = lmesh_neighbors(&net->nodes[i].mesh, nb, LMESH_MAX_NEIGHBORS);
        for (int j = 0; j < SIM_MAX_NODES; j++) {
            snap.link_etx8[i][j] = LMESH_METRIC_INF;
        }
        for (size_t k = 0; k < n; k++) {
            int j = sim_index_of(net, nb[k].id);
            if (j >= 0 && nb[k].up) {
                snap.link_etx8[i][j] = nb[k].etx8;
                snap.link_heard_ms[i][j] = hello_heard_ms[i][j];
            }
        }
    }
}

static const lmesh_route_info *snap_route(const sim_net *net, int src, int dest)
{
    lmesh_node_id id = net->nodes[dest].ident.id;
    for (size_t k = 0; k < snap.count[src]; k++) {
        const lmesh_route_info *r = &snap.rows[src][k];
        if (r->dest == id) {
            return (r->retracted || r->metric8 == LMESH_METRIC_INF || r->next_hop == LMESH_ID_NONE) ? NULL : r;
        }
    }
    return NULL;
}

bool sim_route_get(const sim_net *net, int src, int dest, lmesh_route_info *out)
{
    lmesh_route_info rows[LMESH_MAX_NODES];
    size_t n = lmesh_routes(&net->nodes[src].mesh, rows, LMESH_MAX_NODES);
    for (size_t k = 0; k < n; k++) {
        if (rows[k].dest == net->nodes[dest].ident.id && !rows[k].retracted && rows[k].metric8 != LMESH_METRIC_INF &&
            rows[k].next_hop != LMESH_ID_NONE) {
            if (out) {
                *out = rows[k];
            }
            return true;
        }
    }
    return false;
}

uint32_t sim_clean_window_ms(int hops)
{
    return (uint32_t)(LMESH_ETX_WINDOW + 2 + hops) * (uint32_t)LMESH_HELLO_PERIOD_MS * (100u + LMESH_HELLO_JITTER_PCT) /
           100u;
}

static void note(sim_route_report *rep, const char *fmt, ...)
{
    if (rep->first[0] != '\0') {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rep->first, sizeof rep->first, fmt, ap);
    va_end(ap);
}

typedef enum walk_result { WALK_OK, WALK_MISSING, WALK_INCOMPLETE, WALK_LOOP } walk_result;

static walk_result walk(const sim_net *net, int src, int dest, int chain[SIM_MAX_NODES + 2], int *len, char *why,
                        size_t why_max)
{
    bool visited[SIM_MAX_NODES] = {false};
    bool broken = false;
    int cur = src;
    int hops = 0;
    why[0] = '\0';
    chain[0] = src;
    for (;;) {
        if (cur == dest) {
            *len = hops;
            return broken ? WALK_INCOMPLETE : WALK_OK;
        }
        if (visited[cur] || hops > net->count) {
            *len = hops;
            return WALK_LOOP;
        }
        visited[cur] = true;
        if (!net->nodes[cur].alive) {
            snprintf(why, why_max, "next hop %u is dead", net->nodes[cur].ident.id);
            *len = hops;
            return WALK_INCOMPLETE;
        }
        const lmesh_route_info *r = snap_route(net, cur, dest);
        if (!r) {
            if (cur == src) {
                *len = 0;
                return WALK_MISSING;
            }
            snprintf(why, why_max, "relay %u has no route", net->nodes[cur].ident.id);
            *len = hops;
            return WALK_INCOMPLETE;
        }
        int nh = sim_index_of(net, r->next_hop);
        if (nh < 0) {
            snprintf(why, why_max, "relay %u names unknown next hop %u", net->nodes[cur].ident.id, r->next_hop);
            *len = hops;
            return WALK_INCOMPLETE;
        }
        if (!sim_linked(net, cur, nh) && !broken) {
            broken = true;
            snprintf(why, why_max, "hop %u->%u is not a live link", net->nodes[cur].ident.id, r->next_hop);
        }
        cur = nh;
        hops++;
        chain[hops] = cur;
    }
}

static void chain_text(const sim_net *net, const int *chain, int len, char *out, size_t out_max)
{
    size_t pos = 0;
    out[0] = '\0';
    for (int k = 0; k <= len && pos + 6 < out_max; k++) {
        int w = snprintf(out + pos, out_max - pos, "%s%u", k ? "->" : "", net->nodes[chain[k]].ident.id);
        pos += w > 0 ? (size_t)w : 0u;
    }
}

static void check_hop_sums(const sim_net *net, int dest, const int *chain, int len, sim_route_report *rep)
{
    for (int k = 0; k < len; k++) {
        int x = chain[k];
        int nh = chain[k + 1];
        unsigned expect = snap.link_etx8[x][nh];
        if (nh != dest) {
            const lmesh_route_info *rn = snap_route(net, nh, dest);
            if (!rn || (int32_t)(snap.link_heard_ms[x][nh] - hist.since_ms[nh][dest]) <= 0) {
                continue;
            }
            expect += rn->metric8;
        }
        if (expect > METRIC_MAX_FINITE) {
            expect = METRIC_MAX_FINITE;
        }
        rep->hop_checked++;
        const lmesh_route_info *rx = snap_route(net, x, dest);
        if (rx->metric8 != expect) {
            rep->hop_mismatch++;
            note(rep, "route %u->%u metric %u, link ETX %u plus next hop %u's metric gives %u",
                 net->nodes[x].ident.id, net->nodes[dest].ident.id, rx->metric8, snap.link_etx8[x][nh],
                 net->nodes[nh].ident.id, expect);
        }
    }
}

static void check_metric(const sim_net *net, int src, int dest, int bfs, const int *chain, int len,
                         sim_route_report *rep)
{
    const lmesh_route_info *r = snap_route(net, src, dest);
    int expect = ETX8_ONE * bfs;
    rep->metric_checked++;
    if (len <= SIM_MAX_NODES) {
        check_hop_sums(net, dest, chain, len, rep);
    }
    if (r->metric8 < expect) {
        rep->metric_low++;
        note(rep, "route %u->%u metric %u below %d (bfs %d)", net->nodes[src].ident.id, net->nodes[dest].ident.id,
             r->metric8, expect, bfs);
        return;
    }
    if (len > bfs) {
        rep->nonshortest++;
        char c[96];
        chain_text(net, chain, len, c, sizeof c);
        note(rep, "route %u->%u takes %d hops (%s), bfs %d, metric %u", net->nodes[src].ident.id,
             net->nodes[dest].ident.id, len, c, bfs, r->metric8);
        return;
    }
    if (r->metric8 == expect) {
        return;
    }
    bool clean = true;
    for (int k = 0; k < len && clean; k++) {
        clean = sim_link_clean(net, chain[k], chain[k + 1], sim_clean_window_ms(len));
    }
    if (clean) {
        rep->metric_mismatch++;
        note(rep, "route %u->%u metric %u, expected %d on a clean %d-hop path", net->nodes[src].ident.id,
             net->nodes[dest].ident.id, r->metric8, expect, bfs);
    } else {
        rep->metric_perturbed++;
    }
}

void check_routes(const sim_net *net, bool check_metrics, sim_route_report *rep)
{
    int dist[SIM_MAX_NODES];
    memset(rep, 0, sizeof *rep);
    take_snapshot(net);
    for (int s = 0; s < net->count; s++) {
        if (!net->nodes[s].alive) {
            continue;
        }
        sim_bfs(net, s, -1, dist);
        for (int d = 0; d < net->count; d++) {
            if (d == s) {
                continue;
            }
            bool reachable = net->nodes[d].alive && dist[d] != SIM_UNREACHABLE;
            int chain[SIM_MAX_NODES + 2];
            int len = 0;
            char why[96];
            if (!reachable) {
                if (snap_route(net, s, d)) {
                    rep->phantom++;
                    note(rep, "node %u keeps a route to unreachable %u", net->nodes[s].ident.id,
                         net->nodes[d].ident.id);
                    if (walk(net, s, d, chain, &len, why, sizeof why) == WALK_LOOP) {
                        char c[96];
                        chain_text(net, chain, len, c, sizeof c);
                        rep->loops++;
                        if (rep->loop_first[0] == '\0') {
                            snprintf(rep->loop_first, sizeof rep->loop_first, "loop toward unreachable %u: %s",
                                     net->nodes[d].ident.id, c);
                        }
                    }
                }
                continue;
            }
            rep->pairs++;
            switch (walk(net, s, d, chain, &len, why, sizeof why)) {
            case WALK_OK:
                rep->ok++;
                if (check_metrics) {
                    check_metric(net, s, d, dist[d], chain, len, rep);
                }
                break;
            case WALK_MISSING:
                rep->missing++;
                note(rep, "node %u has no route to %u", net->nodes[s].ident.id, net->nodes[d].ident.id);
                break;
            case WALK_INCOMPLETE:
                rep->incomplete++;
                note(rep, "route %u->%u incomplete: %s", net->nodes[s].ident.id, net->nodes[d].ident.id, why);
                break;
            case WALK_LOOP: {
                char c[96];
                chain_text(net, chain, len, c, sizeof c);
                rep->loops++;
                if (rep->loop_first[0] == '\0') {
                    snprintf(rep->loop_first, sizeof rep->loop_first, "loop toward %u: %s", net->nodes[d].ident.id, c);
                }
                break;
            }
            }
        }
    }
}

bool report_converged(const sim_route_report *rep)
{
    return rep->missing == 0 && rep->loops == 0 && rep->incomplete == 0;
}

void tracker_reset(sim_tracker *t)
{
    memset(t, 0, sizeof *t);
}

void tracker_sample(sim_tracker *t, bool ok, uint32_t now_ms)
{
    if (ok && !t->ok) {
        t->since_ms = now_ms;
        if (!t->ever) {
            t->first_ms = now_ms;
        }
        t->ever = true;
    }
    t->ok = ok;
}

static const char *const names[M_COUNT] = {
    [M_NODES] = "nodes",
    [M_LINKS] = "links",
    [M_DIAMETER] = "diameter",
    [M_DIAMETER_AFTER] = "diameter_after",
    [M_DISCOVERY_MS] = "discovery_ms",
    [M_ETX_SETTLE_MS] = "etx_settle_ms",
    [M_FIRST_ROUTE_MS] = "first_route_ms",
    [M_FIRST_CONVERGENCE_MS] = "first_convergence_ms",
    [M_CONVERGENCE_MS] = "convergence_ms",
    [M_REROUTE_MS] = "reroute_ms",
    [M_RECOVERY_MS] = "recovery_ms",
    [M_RETRACT_MS] = "retract_ms",
    [M_STALE_DELAY_MS] = "stale_delay_ms",
    [M_STALE_DELAY_MIN_MS] = "stale_delay_min_ms",
    [M_FALSE_STALE] = "false_stale",
    [M_ROUTE_METRIC8] = "route_metric8",
    [M_ROUTE_REQUESTS] = "route_requests",
    [M_ROUTE_REQUESTS_AFTER] = "route_requests_after",
    [M_REQ_ORIGINATED_AFTER] = "req_originated_after",
    [M_ROUTE_LOST_EVENTS] = "route_lost_events",
    [M_ROUTE_CHANGED_EVENTS] = "route_changed_events",
    [M_SRC_ROUTE_LOST] = "src_route_lost",
    [M_SRC_ROUTE_CHANGED] = "src_route_changed",
    [M_HELLO_FRAMES] = "hello_frames",
    [M_STATUS_FRAMES] = "status_frames",
    [M_DATA_FRAMES] = "data_frames",
    [M_AIRTIME_MS] = "airtime_ms",
    [M_HALF_DUPLEX_DROPS] = "half_duplex_drops",
    [M_LOOPS] = "loops",
    [M_LOOP_SAMPLES] = "loop_samples",
    [M_METRIC_LOW] = "metric_low",
    [M_METRIC_MISMATCH] = "metric_mismatch",
    [M_METRIC_PERTURBED] = "metric_perturbed",
    [M_METRIC_SUM_CHECKED] = "metric_sum_checked",
    [M_METRIC_SUM_MISMATCH] = "metric_sum_mismatch",
    [M_NONSHORTEST] = "nonshortest",
    [M_PHANTOM_ROUTES] = "phantom_routes",
    [M_HOP_VIOLATIONS] = "hop_violations",
    [M_DELIVERY_RATIO] = "delivery_ratio",
    [M_LATENCY_MEAN_MS] = "latency_mean_ms",
    [M_LATENCY_MAX_MS] = "latency_max_ms",
    [M_DUPLICATES] = "duplicates",
    [M_STORE_DEPTH_MAX] = "store_depth_max",
    [M_SIM_TIME_MS] = "sim_time_ms",
    [M_MSGS_ORIGINATED] = "msgs_originated",
    [M_MSGS_RECEIVED] = "msgs_received",
    [M_MSGS_DELIVERED] = "msgs_delivered",
    [M_MSGS_EXPIRED] = "msgs_expired",
    [M_MSGS_FAILED] = "msgs_failed",
    [M_MSGS_OPEN] = "msgs_open",
    [M_LATENCY_P50_MS] = "latency_p50_ms",
    [M_LATENCY_P95_MS] = "latency_p95_ms",
    [M_RX_LATENCY_MEAN_MS] = "rx_latency_mean_ms",
    [M_RX_LATENCY_P50_MS] = "rx_latency_p50_ms",
    [M_RX_LATENCY_P95_MS] = "rx_latency_p95_ms",
    [M_RX_LATENCY_MAX_MS] = "rx_latency_max_ms",
    [M_STORE_DEPTH_FINAL] = "store_depth_final",
    [M_RECEIPTS_SENT] = "receipts_sent",
    [M_DATA_FIRST_TX] = "data_first_tx",
    [M_DATA_RETRIES] = "data_retries",
    [M_RESEND_FRAMES] = "resend_frames",
    [M_EVICTIONS] = "evictions",
    [M_EXPIRIES] = "expiries",
    [M_TERMINAL_VIOLATIONS] = "terminal_violations",
    [M_STORE_CUSTODY] = "store_custody",
    [M_STORE_BY_COUNTER] = "store_by_counter",
    [M_STORE_ORIGIN_TERMINAL] = "store_origin_terminal",
    [M_STORE_REFUSED] = "store_refused",
    [M_STORE_DEATH_LOST] = "store_death_lost",
    [M_STORE_UNEXPLAINED] = "store_unexplained",
    [M_STORE_LISTED_MAX] = "store_listed_max",
    [M_LOAD] = "load",
    [M_HANDOVERS] = "handovers",
    [M_CMD_EXECUTIONS] = "cmd_executions",
    [M_HOLD_DATA_FRAMES] = "hold_data_frames",
    [M_EXPIRE_AFTER_MS] = "expire_after_ms",
    [M_HEAL_FIRST_MS] = "heal_first_ms",
    [M_HEAL_LAST_MS] = "heal_last_ms",
    [M_KILL_TO_DELIVERY_MS] = "kill_to_delivery_ms",
    [M_REFUSED_SUBMITS] = "refused_submits",
    [M_EVICTED_ROUTINE] = "evicted_routine",
    [M_EVICTED_NORMAL] = "evicted_normal",
    [M_EVICTED_URGENT] = "evicted_urgent",
};

static const sim_metric_id summary_ids[] = {
    M_NODES,       M_LINKS,      M_DIAMETER,       M_DIAMETER_AFTER, M_DISCOVERY_MS,   M_ETX_SETTLE_MS, M_FIRST_ROUTE_MS,       M_CONVERGENCE_MS,
    M_REROUTE_MS,  M_RECOVERY_MS, M_RETRACT_MS,    M_STALE_DELAY_MS, M_ROUTE_METRIC8,       M_ROUTE_REQUESTS_AFTER,
    M_LOOPS,       M_METRIC_PERTURBED, M_MSGS_ORIGINATED, M_DELIVERY_RATIO, M_MSGS_EXPIRED, M_MSGS_FAILED,
    M_LATENCY_MEAN_MS, M_LATENCY_P95_MS, M_LATENCY_MAX_MS, M_RX_LATENCY_MEAN_MS, M_RX_LATENCY_MAX_MS, M_DUPLICATES,
    M_STORE_DEPTH_MAX, M_RECEIPTS_SENT, M_DATA_RETRIES, M_RESEND_FRAMES, M_EVICTIONS, M_LOAD, M_HANDOVERS,
    M_CMD_EXECUTIONS, M_HOLD_DATA_FRAMES, M_EXPIRE_AFTER_MS, M_HEAL_FIRST_MS, M_HEAL_LAST_MS, M_KILL_TO_DELIVERY_MS,
    M_REFUSED_SUBMITS, M_EVICTED_ROUTINE, M_EVICTED_NORMAL, M_EVICTED_URGENT, M_STORE_UNEXPLAINED};

const char *metric_name(sim_metric_id id)
{
    return (unsigned)id < M_COUNT ? names[id] : "?";
}

bool metric_lower_is_worse(sim_metric_id id)
{
    return id == M_DELIVERY_RATIO;
}

void metric_format(double v, char *out, size_t out_max)
{
    if (v == (double)(long long)v) {
        snprintf(out, out_max, "%lld", (long long)v);
    } else {
        snprintf(out, out_max, "%.4f", v);
    }
}

void metrics_init(sim_metrics *m, const char *scenario, uint32_t seed)
{
    memset(m, 0, sizeof *m);
    m->scenario = scenario;
    m->seed = seed;
    m->passed = true;
}

void metric_set(sim_metrics *m, sim_metric_id id, double value)
{
    m->set[id] = true;
    m->v[id] = value;
}

void metric_max(sim_metrics *m, sim_metric_id id, double value)
{
    if (!m->set[id] || value > m->v[id]) {
        metric_set(m, id, value);
    }
}

void metrics_fail(sim_metrics *m, const char *fmt, ...)
{
    char what[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    if (m->passed) {
        snprintf(m->fail, sizeof m->fail, "%s", what);
    }
    m->passed = false;
    if (m->failures++ < MAX_PRINTED_FAILURES) {
        printf("FAIL %s seed=%lu: %s\n", m->scenario, (unsigned long)m->seed, what);
    }
}

void metrics_collect(sim_metrics *m, const sim_net *net, const sim_obs *o)
{
    uint32_t hello = 0, status = 0, data = 0, req = 0, air = 0;
    for (int i = 0; i < net->count; i++) {
        lmesh_stats st;
        lmesh_get_stats(&net->nodes[i].mesh, &st);
        hello += st.tx_frames[SIM_FT_HELLO];
        data += st.tx_frames[SIM_FT_DATA];
        status += st.tx_frames[SIM_FT_STATUS];
        req += st.tx_frames[SIM_FT_ROUTE_REQ];
        air += st.airtime_ms;
    }
    metric_set(m, M_NODES, net->count);
    metric_set(m, M_HELLO_FRAMES, hello);
    metric_set(m, M_STATUS_FRAMES, status);
    metric_set(m, M_DATA_FRAMES, data);
    metric_set(m, M_ROUTE_REQUESTS, req);
    metric_set(m, M_AIRTIME_MS, air);
    metric_set(m, M_HALF_DUPLEX_DROPS, net->frames_half_duplex);
    metric_set(m, M_HOP_VIOLATIONS, o->hop_violations);
    metric_set(m, M_SIM_TIME_MS, net->now_ms);
    if (o->hop_violations > 0) {
        metrics_fail(m, "%lu DATA frames exceeded the hop bound", (unsigned long)o->hop_violations);
    }
    uint32_t dups = obs_duplicates(o);
    bool messages = o->msg.count > 0 || o->msg.audit.samples > 0;
    if (messages) {
        msgobs_metrics(&o->msg, net, m);
        metric_set(m, M_DUPLICATES, dups);
    }
    if (dups > 0) {
        metric_set(m, M_DUPLICATES, dups);
        metrics_fail(m, "%lu duplicate deliveries to an application", (unsigned long)dups);
    }
}

void metrics_print(const sim_metrics *m)
{
    char v[32];
    for (int i = 0; i < M_COUNT; i++) {
        if (m->set[i]) {
            metric_format(m->v[i], v, sizeof v);
            printf("metric %s=%s\n", names[i], v);
        }
    }
}

void metrics_summary(const sim_metrics *m)
{
    char v[32];
    printf("%s %s seed=%lu", m->passed ? "PASS" : "FAIL", m->scenario, (unsigned long)m->seed);
    for (size_t k = 0; k < sizeof summary_ids / sizeof summary_ids[0]; k++) {
        sim_metric_id id = summary_ids[k];
        if (m->set[id]) {
            metric_format(m->v[id], v, sizeof v);
            printf(" %s=%s", names[id], v);
        }
    }
    if (!m->passed) {
        printf(" first_failure=\"%s\"", m->fail);
    }
    printf("\n");
}

int metrics_csv_append(const sim_metrics *m, const char *path)
{
    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
        fprintf(f, "scenario,seed,passed");
        for (int i = 0; i < M_COUNT; i++) {
            fprintf(f, ",%s", names[i]);
        }
        fprintf(f, ",store_depth_max_by_node,store_depth_final_by_node\n");
    }
    fprintf(f, "%s,%lu,%d", m->scenario, (unsigned long)m->seed, m->passed ? 1 : 0);
    char v[32];
    for (int i = 0; i < M_COUNT; i++) {
        if (m->set[i]) {
            metric_format(m->v[i], v, sizeof v);
            fprintf(f, ",%s", v);
        } else {
            fprintf(f, ",");
        }
    }
    fprintf(f, ",%s,%s\n", m->depth_max_by_node, m->depth_final_by_node);
    fclose(f);
    return 0;
}
