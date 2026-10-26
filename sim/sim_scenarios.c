/* Simulator scenarios S1-S14 from PLAN.md Section 5, plus the smoke run. */
#include "sim_scenarios.h"

#include <string.h>

#define SAMPLE_MS 1000u
#define FINE_SAMPLE_MS 100u
#define HELLO_MAX_MS ((uint32_t)LMESH_HELLO_PERIOD_MS * (100u + LMESH_HELLO_JITTER_PCT) / 100u)
#define DISCOVERY_BOUND_MS (2u * HELLO_MAX_MS)
#define FIRST_ROUTE_BOUND_MS (3u * HELLO_MAX_MS)
#define REROUTE_BOUND_MS ((uint32_t)LMESH_ROUTE_HOLD_MS + 2u * HELLO_MAX_MS)
#define STALE_BOUND_MS ((uint32_t)LMESH_NODE_STALE_MS + LMESH_STATUS_PERIOD_MS + LMESH_STATUS_JITTER_MS)
#define ROUTING_RUN_MS 400000u
#define PRE_FAILURE_MS 240000u
#define POST_FAILURE_MS 240000u
#define STATUS_RUN_MS 600000u
#define SETTLE_MS 150000u
#define SETTLE_CAP_MS 300000u
#define NETWORK_ID 1u

static sim_run *cur;

typedef struct sampler {
    sim_tracker conv;
    sim_tracker retract;
    sim_route_report last;
    int loops;
    int loop_samples;
    bool loop_reported;
} sampler;

static void on_event(void *ctx, const lmesh_event *ev)
{
    const sim_node *sn = (const sim_node *)ctx;
    obs_event(&cur->obs, sn->ctx.index, ev);
}

typedef void (*command_fn)(void *ctx, lmesh_node_id from, const uint8_t *data, size_t len, lmesh_cmd_result *result);

static void build_with(sim_run *r, const sim_topo *t, command_fn on_command)
{
    for (int i = 0; i < t->n; i++) {
        if (sim_add_node(&r->net, (lmesh_node_id)(i + 1), NETWORK_ID, on_event, on_command) < 0) {
            metrics_fail(&r->m, "cannot add node %d", i + 1);
        }
    }
    topo_apply(&r->net, t, 0);
    r->topo = *t;
    metric_set(&r->m, M_LINKS, topo_edge_count(t));
    metric_set(&r->m, M_DIAMETER, sim_diameter(&r->net));
    if (r->verbose) {
        char desc[512];
        topo_describe(t, desc, sizeof desc);
        printf("topology %s\n", desc);
    }
}

static void build(sim_run *r, const sim_topo *t)
{
    build_with(r, t, NULL);
}

static void sampler_init(sampler *s)
{
    memset(s, 0, sizeof *s);
}

static void sample(sim_run *r, sampler *s)
{
    check_routes(&r->net, false, &s->last);
    if (s->last.loops > 0) {
        s->loops += s->last.loops;
        s->loop_samples++;
        if (!s->loop_reported) {
            s->loop_reported = true;
            metrics_fail(&r->m, "t=%lu ms %s", (unsigned long)r->net.now_ms, s->last.loop_first);
        }
    }
    tracker_sample(&s->conv, report_converged(&s->last), r->net.now_ms);
    tracker_sample(&s->retract, s->last.phantom == 0, r->net.now_ms);
}

static void step_sampled(sim_run *r, sampler *s, uint32_t ms)
{
    sim_step(&r->net, ms);
    sample(r, s);
}

static void run_to(sim_run *r, sampler *s, uint32_t until_ms)
{
    while ((int32_t)(until_ms - r->net.now_ms) > 0) {
        uint32_t left = until_ms - r->net.now_ms;
        step_sampled(r, s, left < SAMPLE_MS ? left : SAMPLE_MS);
    }
}

static void settle(sim_run *r, sampler *s)
{
    uint32_t start = r->net.now_ms;
    while (!(s->conv.ok && r->net.now_ms - s->conv.since_ms >= SETTLE_MS) && r->net.now_ms - start < SETTLE_CAP_MS) {
        step_sampled(r, s, SAMPLE_MS);
    }
}

static void finish_sampler(sim_run *r, const sampler *s)
{
    metric_set(&r->m, M_LOOPS, s->loops);
    metric_set(&r->m, M_LOOP_SAMPLES, s->loop_samples);
}

static void final_routes(sim_run *r, const char *phase)
{
    sim_route_report rep;
    check_routes(&r->net, true, &rep);
    metric_set(&r->m, M_METRIC_LOW, rep.metric_low);
    metric_set(&r->m, M_METRIC_MISMATCH, rep.metric_mismatch);
    metric_set(&r->m, M_METRIC_PERTURBED, rep.metric_perturbed);
    metric_set(&r->m, M_NONSHORTEST, rep.nonshortest);
    metric_set(&r->m, M_PHANTOM_ROUTES, rep.phantom);
    metric_set(&r->m, M_METRIC_SUM_CHECKED, rep.hop_checked);
    metric_set(&r->m, M_METRIC_SUM_MISMATCH, rep.hop_mismatch);
    bool bad = rep.phantom > 0 || rep.metric_low > 0 || rep.metric_mismatch > 0 || rep.nonshortest > 0 ||
               rep.hop_mismatch > 0;
    if (report_converged(&rep) && bad) {
        metrics_fail(&r->m,
                     "%s: route check at t=%lu ms (%d phantom, %d low, %d mismatch, %d sum mismatch, %d nonshortest): %s",
                     phase, (unsigned long)r->net.now_ms, rep.phantom, rep.metric_low, rep.metric_mismatch,
                     rep.hop_mismatch, rep.nonshortest, rep.first);
    }
}

static bool converged_since(sim_run *r, const sampler *s, uint32_t t0, sim_metric_id id, const char *what)
{
    if (!s->conv.ok) {
        metrics_fail(&r->m, "%s: not converged at t=%lu ms (%d missing, %d incomplete, %d loops): %s", what,
                     (unsigned long)r->net.now_ms, s->last.missing, s->last.incomplete, s->last.loops, s->last.first);
        return false;
    }
    metric_set(&r->m, id, s->conv.since_ms - t0);
    if (id == M_CONVERGENCE_MS) {
        metric_set(&r->m, M_FIRST_CONVERGENCE_MS, s->conv.first_ms - t0);
    }
    return true;
}

static uint32_t route_req_frames(const sim_net *net)
{
    uint32_t n = 0;
    for (int i = 0; i < net->count; i++) {
        lmesh_stats st;
        lmesh_get_stats(&net->nodes[i].mesh, &st);
        n += st.tx_frames[SIM_FT_ROUTE_REQ];
    }
    return n;
}

static bool neighbor_get(const sim_net *net, int i, int j, lmesh_neighbor_info *out)
{
    lmesh_neighbor_info rows[LMESH_MAX_NEIGHBORS];
    size_t n = lmesh_neighbors(&net->nodes[i].mesh, rows, LMESH_MAX_NEIGHBORS);
    for (size_t k = 0; k < n; k++) {
        if (rows[k].id == net->nodes[j].ident.id) {
            *out = rows[k];
            return true;
        }
    }
    return false;
}

static bool usable_pair(const sim_net *net, int a, int b)
{
    lmesh_neighbor_info x, y;
    return neighbor_get(net, a, b, &x) && neighbor_get(net, b, a, &y) && x.up && y.up && x.bidirectional &&
           y.bidirectional;
}

static uint8_t pair_etx8(const sim_net *net, int a, int b)
{
    lmesh_neighbor_info x;
    return neighbor_get(net, a, b, &x) && x.up ? x.etx8 : (uint8_t)LMESH_METRIC_INF;
}

static int random_size(sim_run *r)
{
    return 4 + (int)(r->m.seed % 13u);
}

static void random_topology(sim_run *r, sim_topo *t)
{
    int n = random_size(r);
    int extra = (int)(sim_rand(&r->net) % (uint32_t)(n / 2 + 1));
    topo_random(t, n, extra, &r->net);
}

static void scen_smoke(sim_run *r)
{
    sim_topo t;
    topo_line(&t, 4);
    build(r, &t);
    sim_step(&r->net, 60000);
    printf("sim: t=%lu ms sent=%lu delivered=%lu lost=%lu half_duplex=%lu\n", (unsigned long)r->net.now_ms,
           (unsigned long)r->net.frames_sent, (unsigned long)r->net.frames_delivered,
           (unsigned long)r->net.frames_lost, (unsigned long)r->net.frames_half_duplex);
    for (int i = 0; i < r->net.count; i++) {
        lmesh_stats st;
        lmesh_get_stats(&r->net.nodes[i].mesh, &st);
        printf("node %u: alive=%d tx=%lu rx=%lu malformed=%lu\n", r->net.nodes[i].ident.id, r->net.nodes[i].alive,
               (unsigned long)r->net.nodes[i].frames_tx, (unsigned long)r->net.nodes[i].frames_rx,
               (unsigned long)st.rx_malformed);
    }
}

static void scen_s1(sim_run *r)
{
    sim_topo t;
    topo_line(&t, 2);
    build(r, &t);

    bool found = false;
    while (!found && r->net.now_ms < 3u * DISCOVERY_BOUND_MS) {
        sim_step(&r->net, FINE_SAMPLE_MS);
        found = usable_pair(&r->net, 0, 1);
    }
    if (!found) {
        metrics_fail(&r->m, "nodes did not discover each other within %lu ms", (unsigned long)r->net.now_ms);
        return;
    }
    metric_set(&r->m, M_DISCOVERY_MS, r->net.now_ms);
    if (r->net.now_ms > DISCOVERY_BOUND_MS) {
        metrics_fail(&r->m, "discovery took %lu ms, bound %lu ms", (unsigned long)r->net.now_ms,
                     (unsigned long)DISCOVERY_BOUND_MS);
    }

    sampler s;
    sim_tracker etx;
    sampler_init(&s);
    tracker_reset(&etx);
    while (r->net.now_ms < ROUTING_RUN_MS) {
        step_sampled(r, &s, SAMPLE_MS);
        tracker_sample(&etx, pair_etx8(&r->net, 0, 1) == 8 && pair_etx8(&r->net, 1, 0) == 8, r->net.now_ms);
    }
    settle(r, &s);
    finish_sampler(r, &s);
    converged_since(r, &s, 0, M_CONVERGENCE_MS, "end");
    if (!usable_pair(&r->net, 0, 1)) {
        metrics_fail(&r->m, "link not bidirectional at the end");
    }
    bool clean = sim_link_clean(&r->net, 0, 1, sim_clean_window_ms(1));
    if (etx.ok) {
        metric_set(&r->m, M_ETX_SETTLE_MS, etx.since_ms);
    } else if (clean) {
        metrics_fail(&r->m, "ETX %u/%u at the ends of a clean link, expected 8 (1.0)", pair_etx8(&r->net, 0, 1),
                     pair_etx8(&r->net, 1, 0));
    }
    final_routes(r, "end");
}

static void scen_s2(sim_run *r)
{
    sim_topo t;
    topo_line(&t, 4);
    build(r, &t);

    sampler s;
    sampler_init(&s);
    bool first = false;
    lmesh_route_info ri;
    while (r->net.now_ms < ROUTING_RUN_MS) {
        step_sampled(r, &s, SAMPLE_MS);
        if (!first && sim_route_get(&r->net, 0, 3, &ri) && ri.next_hop == r->net.nodes[1].ident.id) {
            first = true;
            metric_set(&r->m, M_FIRST_ROUTE_MS, r->net.now_ms);
        }
    }
    settle(r, &s);
    finish_sampler(r, &s);
    if (!first) {
        metrics_fail(&r->m, "node 1 never had a route to 4 through 2");
    } else if (r->m.v[M_FIRST_ROUTE_MS] > FIRST_ROUTE_BOUND_MS) {
        metrics_fail(&r->m, "route 1->4 through 2 took %.0f ms, bound %lu ms", r->m.v[M_FIRST_ROUTE_MS],
                     (unsigned long)FIRST_ROUTE_BOUND_MS);
    }
    converged_since(r, &s, 0, M_CONVERGENCE_MS, "end");
    if (sim_route_get(&r->net, 0, 3, &ri)) {
        metric_set(&r->m, M_ROUTE_METRIC8, ri.metric8);
    }
    final_routes(r, "end");
}

static void scen_s3(sim_run *r)
{
    sim_topo t;
    random_topology(r, &t);
    build(r, &t);
    sampler s;
    sampler_init(&s);
    run_to(r, &s, ROUTING_RUN_MS);
    settle(r, &s);
    finish_sampler(r, &s);
    converged_since(r, &s, 0, M_CONVERGENCE_MS, "end");
    final_routes(r, "end");
}

typedef struct failure_watch {
    uint32_t t_fail;
    uint32_t req_before;
    uint32_t orig_before;
} failure_watch;

static void failure_begin(sim_run *r, sampler *s, failure_watch *w)
{
    w->t_fail = r->net.now_ms;
    w->req_before = route_req_frames(&r->net);
    w->orig_before = r->obs.req_originated;
    obs_clear_route_events(&r->obs);
    tracker_reset(&s->conv);
    tracker_reset(&s->retract);
}

static void failure_end(sim_run *r, sampler *s, failure_watch *w, bool node_died)
{
    settle(r, s);
    finish_sampler(r, s);
    metric_set(&r->m, M_DIAMETER_AFTER, sim_diameter(&r->net));
    converged_since(r, s, w->t_fail, M_RECOVERY_MS, "after failure");
    if (node_died) {
        if (s->retract.ok) {
            metric_set(&r->m, M_RETRACT_MS, s->retract.since_ms - w->t_fail);
        } else {
            metrics_fail(&r->m, "routes to the dead node remain at t=%lu ms: %s", (unsigned long)r->net.now_ms,
                         s->last.first);
        }
    }
    uint32_t lost = 0, changed = 0;
    for (int i = 0; i < r->net.count; i++) {
        for (int d = 0; d < r->net.count; d++) {
            lost += r->obs.route_lost[i][d];
            changed += r->obs.route_changed[i][d];
        }
    }
    metric_set(&r->m, M_ROUTE_LOST_EVENTS, lost);
    metric_set(&r->m, M_ROUTE_CHANGED_EVENTS, changed);
    metric_set(&r->m, M_ROUTE_REQUESTS_AFTER, route_req_frames(&r->net) - w->req_before);
    metric_set(&r->m, M_REQ_ORIGINATED_AFTER, r->obs.req_originated - w->orig_before);
    final_routes(r, "after failure");
}

static void diamond_failure(sim_run *r, bool cut_link)
{
    const int a = 0, d = 3;
    sim_topo t;
    topo_diamond(&t);
    build(r, &t);

    sampler s;
    sampler_init(&s);
    run_to(r, &s, PRE_FAILURE_MS);
    settle(r, &s);
    if (!converged_since(r, &s, 0, M_CONVERGENCE_MS, "before failure")) {
        finish_sampler(r, &s);
        return;
    }
    lmesh_route_info ri;
    sim_route_get(&r->net, a, d, &ri);
    int relay = sim_index_of(&r->net, ri.next_hop);
    if (relay != 1 && relay != 2) {
        metrics_fail(&r->m, "route 1->4 uses next hop %u, expected 2 or 3", ri.next_hop);
        finish_sampler(r, &s);
        return;
    }
    int other = relay == 1 ? 2 : 1;
    lmesh_node_id other_id = r->net.nodes[other].ident.id;
    if (r->verbose) {
        printf("failure at t=%lu ms: %s %u\n", (unsigned long)r->net.now_ms, cut_link ? "cut link 1 -" : "kill",
               r->net.nodes[relay].ident.id);
    }

    failure_watch w;
    failure_begin(r, &s, &w);
    if (cut_link) {
        sim_set_link(&r->net, a, relay, false, 0);
    } else {
        sim_kill(&r->net, relay);
    }

    bool moved = false;
    while (r->net.now_ms - w.t_fail < POST_FAILURE_MS) {
        step_sampled(r, &s, SAMPLE_MS);
        if (!moved && sim_route_get(&r->net, a, d, &ri) && ri.next_hop == other_id) {
            moved = true;
            metric_set(&r->m, M_REROUTE_MS, r->net.now_ms - w.t_fail);
        }
    }
    if (!moved) {
        metrics_fail(&r->m, "route 1->4 never moved to %u", other_id);
    } else if (r->m.v[M_REROUTE_MS] > REROUTE_BOUND_MS) {
        metrics_fail(&r->m, "route 1->4 moved after %.0f ms, bound %lu ms", r->m.v[M_REROUTE_MS],
                     (unsigned long)REROUTE_BOUND_MS);
    }
    metric_set(&r->m, M_SRC_ROUTE_LOST, r->obs.route_lost[a][d]);
    metric_set(&r->m, M_SRC_ROUTE_CHANGED, r->obs.route_changed[a][d]);
    if (r->obs.route_changed[a][d] == 0 || r->obs.last_hop[a][d] != other_id) {
        metrics_fail(&r->m, "no route-changed event at 1 for 4 through %u (lost=%u changed=%u)", other_id,
                     r->obs.route_lost[a][d], r->obs.route_changed[a][d]);
    }
    failure_end(r, &s, &w, !cut_link);
}

static void scen_s4(sim_run *r)
{
    diamond_failure(r, false);
}

static void scen_s5(sim_run *r)
{
    diamond_failure(r, true);
}

static void scen_s6(sim_run *r)
{
    sim_topo t;
    random_topology(r, &t);
    build(r, &t);
    int victim = sim_kill_candidate(&r->net, true);
    if (victim < 0) {
        metrics_fail(&r->m, "no node can be killed without partitioning the network");
        return;
    }

    sampler s;
    sampler_init(&s);
    run_to(r, &s, PRE_FAILURE_MS);
    settle(r, &s);
    if (!converged_since(r, &s, 0, M_CONVERGENCE_MS, "before failure")) {
        finish_sampler(r, &s);
        return;
    }
    if (r->verbose) {
        printf("failure at t=%lu ms: kill %u\n", (unsigned long)r->net.now_ms, r->net.nodes[victim].ident.id);
    }
    failure_watch w;
    failure_begin(r, &s, &w);
    sim_kill(&r->net, victim);
    while (r->net.now_ms - w.t_fail < POST_FAILURE_MS) {
        step_sampled(r, &s, SAMPLE_MS);
    }
    failure_end(r, &s, &w, true);
}

static int find_node_entry(const lmesh_node_info *rows, size_t n, lmesh_node_id id)
{
    for (size_t k = 0; k < n; k++) {
        if (rows[k].id == id) {
            return (int)k;
        }
    }
    return -1;
}

static void check_node_tables(sim_run *r)
{
    sim_net *net = &r->net;
    for (int i = 0; i < net->count; i++) {
        if (!net->nodes[i].alive) {
            continue;
        }
        lmesh_node_info rows[LMESH_MAX_NODES];
        size_t n = lmesh_nodes(&net->nodes[i].mesh, rows, LMESH_MAX_NODES);
        for (int j = 0; j < net->count; j++) {
            if (j == i || !net->nodes[j].alive) {
                continue;
            }
            int k = find_node_entry(rows, n, net->nodes[j].ident.id);
            if (k < 0) {
                metrics_fail(&r->m, "node %u has no table entry for %u", net->nodes[i].ident.id,
                             net->nodes[j].ident.id);
                continue;
            }
            if (rows[k].stale) {
                metrics_fail(&r->m, "node %u shows %u stale", net->nodes[i].ident.id, net->nodes[j].ident.id);
            }
            bool listed[SIM_MAX_NODES] = {false};
            bool match = rows[k].neighbor_count > 0;
            for (size_t q = 0; q < rows[k].neighbor_count; q++) {
                int x = sim_index_of(net, rows[k].neighbors[q]);
                if (x < 0 || !sim_linked(net, j, x) || listed[x]) {
                    match = false;
                } else {
                    listed[x] = true;
                }
            }
            for (int x = 0; x < net->count; x++) {
                if (sim_linked(net, j, x) && !listed[x]) {
                    match = false;
                }
            }
            if (!match) {
                metrics_fail(&r->m, "node %u's table lists %u neighbors for %u, topology disagrees",
                             net->nodes[i].ident.id, rows[k].neighbor_count, net->nodes[j].ident.id);
            }
        }
    }
}

static bool shows_stale(const sim_net *net, int observer, int subject, bool *present)
{
    lmesh_node_info rows[LMESH_MAX_NODES];
    size_t n = lmesh_nodes(&net->nodes[observer].mesh, rows, LMESH_MAX_NODES);
    int k = find_node_entry(rows, n, net->nodes[subject].ident.id);
    *present = k >= 0;
    return k >= 0 && rows[k].stale;
}

static void scen_s13(sim_run *r)
{
    const int n = 6;
    sim_topo t;
    topo_ring(&t, n);
    topo_edge(&t, 0, 3);
    for (int tries = 0; tries < 100; tries++) {
        int a = (int)(sim_rand(&r->net) % (uint32_t)n);
        int b = (int)(sim_rand(&r->net) % (uint32_t)n);
        if (a != b && !t.adj[a][b]) {
            topo_edge(&t, a, b);
            break;
        }
    }
    int victim = (int)(sim_rand(&r->net) % (uint32_t)n);
    build(r, &t);

    sampler s;
    sampler_init(&s);
    run_to(r, &s, STATUS_RUN_MS);
    settle(r, &s);
    converged_since(r, &s, 0, M_CONVERGENCE_MS, "before kill");
    final_routes(r, "before kill");
    check_node_tables(r);

    uint32_t t_kill = r->net.now_ms;
    uint32_t delay[SIM_MAX_NODES] = {0};
    bool seen[SIM_MAX_NODES] = {false};
    bool false_stale[SIM_MAX_NODES][SIM_MAX_NODES] = {{false}};
    int false_stale_pairs = 0;
    if (r->verbose) {
        printf("kill %u at t=%lu ms\n", r->net.nodes[victim].ident.id, (unsigned long)t_kill);
    }
    sim_kill(&r->net, victim);

    int pending = n - 1;
    while (pending > 0 && r->net.now_ms - t_kill <= STALE_BOUND_MS + SAMPLE_MS) {
        step_sampled(r, &s, SAMPLE_MS);
        for (int i = 0; i < n; i++) {
            bool present;
            if (i == victim) {
                continue;
            }
            for (int j = 0; j < n; j++) {
                if (j != i && j != victim && !false_stale[i][j] && shows_stale(&r->net, i, j, &present)) {
                    false_stale[i][j] = true;
                    false_stale_pairs++;
                    if (r->verbose) {
                        printf("node %u shows live node %u stale at t=%lu ms\n", r->net.nodes[i].ident.id,
                               r->net.nodes[j].ident.id, (unsigned long)r->net.now_ms);
                    }
                }
            }
            if (seen[i]) {
                continue;
            }
            if (shows_stale(&r->net, i, victim, &present)) {
                seen[i] = true;
                delay[i] = r->net.now_ms - t_kill;
                pending--;
            } else if (!present) {
                seen[i] = true;
                pending--;
                metrics_fail(&r->m, "node %u lost its table entry for %u", r->net.nodes[i].ident.id,
                             r->net.nodes[victim].ident.id);
            }
        }
    }
    metric_set(&r->m, M_FALSE_STALE, false_stale_pairs);
    finish_sampler(r, &s);
    uint32_t worst = 0, best = UINT32_MAX;
    for (int i = 0; i < n; i++) {
        if (i == victim) {
            continue;
        }
        if (!seen[i]) {
            metrics_fail(&r->m, "node %u did not mark %u stale within %lu ms", r->net.nodes[i].ident.id,
                         r->net.nodes[victim].ident.id, (unsigned long)STALE_BOUND_MS);
            continue;
        }
        if (delay[i] > 0) {
            worst = delay[i] > worst ? delay[i] : worst;
            best = delay[i] < best ? delay[i] : best;
        }
    }
    if (worst > 0) {
        metric_set(&r->m, M_STALE_DELAY_MS, worst);
        metric_set(&r->m, M_STALE_DELAY_MIN_MS, best);
    }
    if (worst > STALE_BOUND_MS) {
        metrics_fail(&r->m, "stale after %lu ms, bound %lu ms", (unsigned long)worst, (unsigned long)STALE_BOUND_MS);
    }
}

#define MSG_WARM_MS 30000u
#define TRAFFIC_STEP_MS 100u
#define FAST_STEP_MS 20u
#define CMD_RESULT_LEN 4u
#define NODE_A 0
#define NODE_B 1
#define NODE_C 2
#define NODE_D 3
#define ID_A 1u
#define ID_D 4u
#define S7_RUN_MS 120000u
#define S8_RUN_MS 600000u
#define S9_MESSAGES 100u
#define S9_SPREAD_MS 1000u
#define S9_LOSS_PCT 30u
#define S9_LIFETIME_S 600u
#define S9_RUN_MS 900000u
#define S9_ROUTE_WAIT_MS 300000u
#define S10_PER_CLASS 4
#define S10_SHORT_S 30u
#define S10_SHORT_SLACK_MS 2000u
#define S10_DETECT_MS 180000u
#define S10_HOLD_MS 90000u
#define S10_RUN_MS 600000u
#define WRR_SKIP_BOUND 6
#define S11_WARM_MIN 8u
#define S11_WARM_SPREAD 8u
#define S11_PHASE_MS 120000u
#define S11_DRAIN_MS 300000u
#define S11_HANDOVER_BOUND 7u
#define S12_RUN_MS 700000u
#define S12L_DEAD_MS 15000u
#define S14_DEST 4u
#define LONG_LIFETIME_S 600u

static const uint8_t cmd_result_bytes[CMD_RESULT_LEN] = {0xC0, 0xFF, 0xEE, 0x42};
static int cmd_calls;
static lmesh_node_id cmd_from;

typedef struct traffic {
    sampler s;
    uint32_t next_ms;
} traffic;

static void on_command(void *ctx, lmesh_node_id from, const uint8_t *data, size_t len, lmesh_cmd_result *result)
{
    (void)ctx;
    (void)data;
    (void)len;
    cmd_calls++;
    cmd_from = from;
    result->code = LMESH_RESULT_EXECUTED;
    result->len = CMD_RESULT_LEN;
    memcpy(result->data, cmd_result_bytes, CMD_RESULT_LEN);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static void audit_iter(void *ctx, sim_net *net)
{
    msgobs_sample((sim_msgobs *)ctx, net);
}

static void audit_start(sim_run *r)
{
    r->net.on_iter = audit_iter;
    r->net.iter_ctx = &r->obs.msg;
    msgobs_sample(&r->obs.msg, &r->net);
}

static void traffic_audit(sim_run *r, traffic *tr)
{
    sample(r, &tr->s);
    tr->next_ms = r->net.now_ms + SAMPLE_MS;
}

static void traffic_step(sim_run *r, traffic *tr, uint32_t ms)
{
    while (ms > 0) {
        uint32_t to_sample = tr->next_ms - r->net.now_ms;
        uint32_t chunk = (int32_t)to_sample <= 0 ? 1u : (to_sample < ms ? to_sample : ms);
        sim_step(&r->net, chunk);
        ms -= chunk;
        if ((int32_t)(r->net.now_ms - tr->next_ms) >= 0) {
            traffic_audit(r, tr);
        }
    }
}

static bool msg_setup(sim_run *r, traffic *tr, const sim_topo *t, uint8_t loss_pct, bool commands)
{
    build_with(r, t, commands ? on_command : NULL);
    if (loss_pct > 0) {
        topo_apply(&r->net, t, loss_pct);
    }
    cmd_calls = 0;
    cmd_from = LMESH_ID_NONE;
    sampler_init(&tr->s);
    run_to(r, &tr->s, MSG_WARM_MS);
    settle(r, &tr->s);
    bool ok = loss_pct > 0 ? true : converged_since(r, &tr->s, 0, M_CONVERGENCE_MS, "before traffic");
    if (loss_pct > 0 && tr->s.conv.ok) {
        metric_set(&r->m, M_CONVERGENCE_MS, tr->s.conv.since_ms);
    }
    audit_start(r);
    traffic_audit(r, tr);
    return ok;
}

static sim_track *submit(sim_run *r, int node, lmesh_node_id dest, lmesh_msg_type type, lmesh_prio prio,
                         uint32_t lifetime_s, const char *text)
{
    lmesh_msg_ref ref;
    lmesh_err e = lmesh_send(&r->net.nodes[node].mesh, dest, type, prio, lifetime_s, (const uint8_t *)text,
                             strlen(text), &ref);
    if (e != LMESH_OK) {
        metrics_fail(&r->m, "t=%lu ms submit at %u refused with error %d", (unsigned long)r->net.now_ms,
                     r->net.nodes[node].ident.id, (int)e);
        return NULL;
    }
    sim_track *t = msgobs_find(&r->obs.msg, ref);
    if (!t) {
        metrics_fail(&r->m, "submit of %u/%u raised no Queued event", ref.origin, ref.id);
    }
    return t;
}

static bool track_states(const sim_track *t, const uint8_t *states, const uint8_t *reasons, uint8_t n)
{
    if (t->n_states != n) {
        return false;
    }
    for (uint8_t k = 0; k < n; k++) {
        if (t->states[k] != states[k] || t->reasons[k] != reasons[k]) {
            return false;
        }
    }
    return true;
}

static void state_text(const sim_track *t, char *out, size_t out_max)
{
    size_t pos = 0;
    out[0] = '\0';
    for (uint8_t k = 0; k < t->n_states && pos + 8 < out_max; k++) {
        int w = snprintf(out + pos, out_max - pos, "%s%u/%u", k ? "," : "", t->states[k], t->reasons[k]);
        pos += w > 0 ? (size_t)w : 0u;
    }
}

static void require_once(sim_run *r, const sim_track *t)
{
    if (t->received != 1) {
        metrics_fail(&r->m, "%u/%u delivered %u times at %u", t->ref.origin, t->ref.id, t->received, t->dest);
    }
    if (t->delivered != 1 || t->terminal != 1) {
        metrics_fail(&r->m, "%u/%u: %u Delivered events, %u terminal events at the origin (final %u reason %u)",
                     t->ref.origin, t->ref.id, t->delivered, t->terminal, t->final_state, t->final_reason);
    }
}

typedef struct receipt_path {
    uint16_t id;
    unsigned mask;
} receipt_path;

static void s7_hook(void *ctx, int from, int to, const uint8_t *frame, size_t len)
{
    receipt_path *w = (receipt_path *)ctx;
    sim_data_view v;
    if (to == SIM_TO_AIR || !sim_parse_data(frame, len, &v) || v.type != LMESH_MSG_RECEIPT) {
        return;
    }
    if (v.ref.origin != ID_D || v.dest != ID_A || v.content_len < 3 || le16(v.content) != w->id ||
        v.rx != cur->net.nodes[to].ident.id) {
        return;
    }
    if (to >= NODE_A && to < NODE_D && from == to + 1) {
        w->mask |= 1u << to;
    }
}

static void scen_s7(sim_run *r)
{
    static const uint8_t want_states[] = {LMESH_STATE_QUEUED, LMESH_STATE_TRANSMITTED, LMESH_STATE_DELIVERED};
    static const uint8_t want_reasons[] = {LMESH_REASON_NONE, LMESH_REASON_HOP_ACCEPTED, LMESH_REASON_RECEIPT};
    static receipt_path w;
    traffic tr;
    sim_topo t;
    topo_line(&t, 4);
    if (!msg_setup(r, &tr, &t, 0, false)) {
        return;
    }
    memset(&w, 0, sizeof w);
    r->obs.user_hook = s7_hook;
    r->obs.user_ctx = &w;
    sim_track *tk = submit(r, NODE_A, ID_D, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, LONG_LIFETIME_S, "S7 text from 1");
    if (!tk) {
        return;
    }
    w.id = tk->ref.id;
    uint32_t t0 = r->net.now_ms;
    while (r->net.now_ms - t0 < S7_RUN_MS && !tk->terminal) {
        traffic_step(r, &tr, TRAFFIC_STEP_MS);
    }
    traffic_step(r, &tr, SAMPLE_MS);
    char seq[96];
    state_text(tk, seq, sizeof seq);
    if (!track_states(tk, want_states, want_reasons, 3)) {
        metrics_fail(&r->m, "events at 1 were %s (state/reason), expected 0/0,1/1,2/2", seq);
    }
    require_once(r, tk);
    if (tk->rx_hops != 3) {
        metrics_fail(&r->m, "delivered at 4 with hop count %u, expected 3", tk->rx_hops);
    }
    if (tk->result_code != LMESH_RESULT_DELIVERED) {
        metrics_fail(&r->m, "receipt result %u, expected DELIVERED", tk->result_code);
    }
    if (w.mask != 7u) {
        metrics_fail(&r->m, "receipt path incomplete: hop mask %u (bit 2 = 4->3, bit 1 = 3->2, bit 0 = 2->1)", w.mask);
    }
    final_routes(r, "after traffic");
}

typedef struct cmd_cut {
    uint16_t id;
    bool seen;
    bool cut;
    bool restored;
    uint32_t cut_ms;
} cmd_cut;

static void s8_hook(void *ctx, int from, int to, const uint8_t *frame, size_t len)
{
    cmd_cut *w = (cmd_cut *)ctx;
    sim_data_view v;
    if (w->restored || !sim_parse_data(frame, len, &v) || v.ref.origin != ID_A || v.ref.id != w->id ||
        v.rx != ID_D) {
        return;
    }
    if (to == NODE_D && from == NODE_C && !w->seen) {
        w->seen = true;
        w->cut = true;
        w->cut_ms = cur->net.now_ms;
        sim_set_loss_dir(&cur->net, NODE_D, NODE_C, 100);
    } else if (to == SIM_TO_AIR && from == NODE_C && w->cut && !v.retry) {
        w->restored = true;
        sim_set_loss_dir(&cur->net, NODE_D, NODE_C, 0);
    }
}

static uint32_t receipts_for(const sim_msgobs *mo, uint16_t id, const sim_receipt **first)
{
    uint32_t n = 0;
    *first = NULL;
    for (uint32_t k = 0; k < mo->receipt_count; k++) {
        const sim_receipt *rc = &mo->receipts[k];
        if (rc->ref.origin == ID_D && rc->dest == ID_A && rc->orig_id == id) {
            n++;
            if (!*first || (int32_t)(rc->first_ms - (*first)->first_ms) < 0) {
                *first = rc;
            }
        }
    }
    return n;
}

static void scen_s8(sim_run *r)
{
    static cmd_cut w;
    traffic tr;
    sim_topo t;
    topo_line(&t, 4);
    if (!msg_setup(r, &tr, &t, 0, true)) {
        return;
    }
    memset(&w, 0, sizeof w);
    r->obs.user_hook = s8_hook;
    r->obs.user_ctx = &w;
    sim_track *tk = submit(r, NODE_A, ID_D, LMESH_MSG_COMMAND, LMESH_PRIO_NORMAL, 0, "S8 command");
    if (!tk) {
        return;
    }
    w.id = tk->ref.id;
    const sim_receipt *first = NULL;
    lmesh_stats st;
    uint32_t t0 = r->net.now_ms;
    for (;;) {
        lmesh_get_stats(&r->net.nodes[NODE_D].mesh, &st);
        bool done = tk->terminal && st.dup_dest > 0 && receipts_for(&r->obs.msg, w.id, &first) >= 2;
        if (done || r->net.now_ms - t0 >= S8_RUN_MS) {
            break;
        }
        traffic_step(r, &tr, FAST_STEP_MS);
    }
    traffic_step(r, &tr, SAMPLE_MS);
    lmesh_get_stats(&r->net.nodes[NODE_D].mesh, &st);
    uint32_t n_receipts = receipts_for(&r->obs.msg, w.id, &first);
    metric_set(&r->m, M_CMD_EXECUTIONS, cmd_calls);
    if (!w.restored) {
        metrics_fail(&r->m, "3 never handed the command to 4 a second time after its hop ACKs were lost");
    }
    if (cmd_calls != 1 || cmd_from != ID_A) {
        metrics_fail(&r->m, "command callback ran %d times (from %u), expected once from 1", cmd_calls, cmd_from);
    }
    require_once(r, tk);
    if (tk->delivered && tk->result_code != LMESH_RESULT_EXECUTED) {
        metrics_fail(&r->m, "Delivered event at 1 carries result %u, expected EXECUTED", tk->result_code);
    }
    if (st.dup_dest == 0) {
        metrics_fail(&r->m, "no second copy of the command reached 4's message layer");
    }
    if (n_receipts < 2) {
        metrics_fail(&r->m, "%lu receipts for the command seen on air, expected at least 2", (unsigned long)n_receipts);
    }
    for (uint32_t k = 0; k < r->obs.msg.receipt_count; k++) {
        const sim_receipt *rc = &r->obs.msg.receipts[k];
        if (rc->ref.origin != ID_D || rc->orig_id != w.id) {
            continue;
        }
        if (rc->code != LMESH_RESULT_EXECUTED) {
            metrics_fail(&r->m, "receipt %u/%u carries result %u, expected EXECUTED", rc->ref.origin, rc->ref.id,
                         rc->code);
        }
        bool is_first = rc == first;
        unsigned want_len = is_first ? 3u + CMD_RESULT_LEN : 3u;
        if (rc->len != want_len || (is_first && memcmp(rc->data, cmd_result_bytes, CMD_RESULT_LEN) != 0)) {
            metrics_fail(&r->m, "receipt %u/%u (%s) has %u content bytes, expected %u", rc->ref.origin, rc->ref.id,
                         is_first ? "first" : "replay", rc->len, want_len);
        }
    }
}

static void scen_s9(sim_run *r)
{
    traffic tr;
    sim_topo t;
    static sim_track *tk[S9_MESSAGES];
    uint32_t due[S9_MESSAGES];
    topo_line(&t, 4);
    msg_setup(r, &tr, &t, S9_LOSS_PCT, false);
    uint32_t w0 = r->net.now_ms;
    while (!sim_route_get(&r->net, NODE_A, NODE_D, NULL) && r->net.now_ms - w0 < S9_ROUTE_WAIT_MS) {
        traffic_step(r, &tr, SAMPLE_MS);
    }
    uint32_t t0 = r->net.now_ms;
    for (uint32_t k = 0; k < S9_MESSAGES; k++) {
        due[k] = t0 + k * S9_SPREAD_MS + sim_rand(&r->net) % S9_SPREAD_MS;
        tk[k] = NULL;
    }
    uint32_t next = 0, refused = 0;
    while (r->net.now_ms - t0 < S9_RUN_MS) {
        while (next < S9_MESSAGES && (int32_t)(r->net.now_ms - due[next]) >= 0) {
            char text[16];
            snprintf(text, sizeof text, "S9 %03lu", (unsigned long)next);
            lmesh_msg_ref ref;
            lmesh_err e = lmesh_send(&r->net.nodes[NODE_A].mesh, ID_D, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL,
                                     S9_LIFETIME_S, (const uint8_t *)text, strlen(text), &ref);
            tk[next] = e == LMESH_OK ? msgobs_find(&r->obs.msg, ref) : NULL;
            if (e == LMESH_ERR_FULL) {
                refused++;
            } else if (e != LMESH_OK || !tk[next]) {
                metrics_fail(&r->m, "t=%lu ms submit %lu at 1 returned %d", (unsigned long)r->net.now_ms,
                             (unsigned long)next, (int)e);
            }
            next++;
        }
        if (next == S9_MESSAGES && msgobs_all_terminal(&r->obs.msg)) {
            break;
        }
        traffic_step(r, &tr, TRAFFIC_STEP_MS);
    }
    traffic_step(r, &tr, SAMPLE_MS);
    metric_set(&r->m, M_REFUSED_SUBMITS, refused);
    uint32_t missing = 0, multiple = 0, open = 0;
    for (uint32_t k = 0; k < S9_MESSAGES; k++) {
        if (!tk[k]) {
            continue;
        }
        missing += tk[k]->received == 0 ? 1u : 0u;
        multiple += tk[k]->received > 1 ? 1u : 0u;
        open += tk[k]->terminal ? 0u : 1u;
    }
    if (missing || multiple) {
        metrics_fail(&r->m, "%lu of %lu accepted messages never delivered at 4, %lu delivered more than once",
                     (unsigned long)missing, (unsigned long)(S9_MESSAGES - refused), (unsigned long)multiple);
    }
    if (open) {
        metrics_fail(&r->m, "%lu messages not terminal %lu ms after the first submit", (unsigned long)open,
                     (unsigned long)(r->net.now_ms - t0));
    }
}

static bool neighbor_up(const sim_net *net, int i, int j)
{
    lmesh_neighbor_info x;
    return neighbor_get(net, i, j, &x) && x.up;
}

static char prio_letter(unsigned p)
{
    return p == LMESH_PRIO_URGENT ? 'U' : p == LMESH_PRIO_NORMAL ? 'N' : 'R';
}

static void check_wrr_order(sim_run *r, sim_track *const *tk, int n)
{
    sim_track *order[3 * S10_PER_CLASS];
    int m = 0;
    for (int k = 0; k < n; k++) {
        if (tk[k] && tk[k]->tx_seen) {
            int at = m++;
            while (at > 0 && order[at - 1]->handover_index > tk[k]->handover_index) {
                order[at] = order[at - 1];
                at--;
            }
            order[at] = tk[k];
        }
    }
    char text[3 * S10_PER_CLASS + 1];
    int last[LMESH_PRIO_COUNT] = {-1, -1, -1};
    for (int p = 0; p < m; p++) {
        uint8_t c = order[p]->prio;
        text[p] = prio_letter(c);
        bool higher_waiting = false;
        for (int q = p + 1; q < m; q++) {
            higher_waiting = higher_waiting || order[q]->prio < c;
        }
        for (int k = 0; k < n; k++) {
            higher_waiting = higher_waiting || (tk[k] && !tk[k]->tx_seen && tk[k]->prio < c);
        }
        int skipped = p - last[c] - 1;
        if (higher_waiting && skipped < WRR_SKIP_BOUND) {
            metrics_fail(&r->m, "handover %d is %c while a higher class waits, after only %d skips", p + 1,
                         prio_letter(c), skipped);
        }
        last[c] = p;
    }
    text[m] = '\0';
    if (r->verbose || !r->m.passed) {
        printf("handover order after heal: %s\n", text);
    }
    if (m < n) {
        metrics_fail(&r->m, "only %d of %d held messages were handed over after the heal", m, n);
    }
}

static void scen_s10(sim_run *r)
{
    static const lmesh_prio classes[3] = {LMESH_PRIO_ROUTINE, LMESH_PRIO_NORMAL, LMESH_PRIO_URGENT};
    traffic tr;
    sim_topo t;
    sim_track *tk[3 * S10_PER_CLASS];
    topo_diamond(&t);
    if (!msg_setup(r, &tr, &t, 0, false)) {
        return;
    }
    sim_kill(&r->net, NODE_B);
    sim_kill(&r->net, NODE_C);
    uint32_t t_kill = r->net.now_ms;
    while (r->net.now_ms - t_kill < S10_DETECT_MS &&
           (sim_route_get(&r->net, NODE_A, NODE_D, NULL) || neighbor_up(&r->net, NODE_A, NODE_B) ||
            neighbor_up(&r->net, NODE_A, NODE_C))) {
        traffic_step(r, &tr, SAMPLE_MS);
    }
    if (sim_route_get(&r->net, NODE_A, NODE_D, NULL)) {
        metrics_fail(&r->m, "1 still has a route to 4 %lu ms after both relays died",
                     (unsigned long)(r->net.now_ms - t_kill));
        return;
    }
    uint32_t handovers0 = r->obs.msg.handovers[NODE_A];
    uint32_t retries0 = r->obs.msg.data_retries;
    int n = 0;
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < S10_PER_CLASS; k++) {
            char text[24];
            snprintf(text, sizeof text, "S10 %c%d", prio_letter(classes[c]), k);
            tk[n++] = submit(r, NODE_A, ID_D, LMESH_MSG_TEXT, classes[c], LONG_LIFETIME_S, text);
        }
    }
    sim_track *brief = submit(r, NODE_A, ID_D, LMESH_MSG_TEXT, LMESH_PRIO_ROUTINE, S10_SHORT_S, "S10 short");
    if (!brief) {
        return;
    }
    uint32_t t_submit = r->net.now_ms;
    while (r->net.now_ms - t_submit < S10_HOLD_MS) {
        traffic_step(r, &tr, TRAFFIC_STEP_MS);
    }
    uint32_t held_frames = (r->obs.msg.handovers[NODE_A] - handovers0) + (r->obs.msg.data_retries - retries0);
    metric_set(&r->m, M_HOLD_DATA_FRAMES, held_frames);
    if (held_frames > 0) {
        metrics_fail(&r->m, "%lu DATA frames sent while both relays were dead", (unsigned long)held_frames);
    }
    for (int k = 0; k < n; k++) {
        if (tk[k] && (tk[k]->n_states != 1 || tk[k]->terminal)) {
            metrics_fail(&r->m, "%u/%u left Queued while held (state %u)", tk[k]->ref.origin, tk[k]->ref.id,
                         tk[k]->states[tk[k]->n_states - 1]);
        }
    }
    uint32_t expire_after = brief->terminal_ms - brief->submit_ms;
    if (brief->terminal) {
        metric_set(&r->m, M_EXPIRE_AFTER_MS, expire_after);
    }
    if (!brief->terminal || brief->final_state != LMESH_STATE_EXPIRED || brief->final_reason != LMESH_REASON_LIFETIME ||
        expire_after < S10_SHORT_S * 1000u || expire_after > S10_SHORT_S * 1000u + S10_SHORT_SLACK_MS) {
        metrics_fail(&r->m, "30 s message: terminal=%u state %u reason %u after %lu ms, expected Expired/lifetime at 30 s",
                     brief->terminal, brief->final_state, brief->final_reason, (unsigned long)expire_after);
    }

    int relay = (sim_rand(&r->net) & 1u) ? NODE_B : NODE_C;
    sim_revive(&r->net, relay);
    uint32_t t_heal = r->net.now_ms;
    if (r->verbose) {
        printf("revive %u at t=%lu ms\n", r->net.nodes[relay].ident.id, (unsigned long)t_heal);
    }
    for (;;) {
        bool all = true;
        for (int k = 0; k < n; k++) {
            all = all && tk[k] && tk[k]->terminal;
        }
        if (all || r->net.now_ms - t_heal >= S10_RUN_MS) {
            break;
        }
        traffic_step(r, &tr, TRAFFIC_STEP_MS);
    }
    traffic_step(r, &tr, SAMPLE_MS);
    uint32_t first = UINT32_MAX, last = 0;
    for (int k = 0; k < n; k++) {
        if (!tk[k]) {
            continue;
        }
        require_once(r, tk[k]);
        if (tk[k]->received) {
            uint32_t d = tk[k]->received_ms - t_heal;
            first = d < first ? d : first;
            last = d > last ? d : last;
        }
    }
    if (first != UINT32_MAX) {
        metric_set(&r->m, M_HEAL_FIRST_MS, first);
        metric_set(&r->m, M_HEAL_LAST_MS, last);
    }
    check_wrr_order(r, tk, n);
}

static const unsigned s11_loads[] = {2, 4, 8, 16, 32};

static unsigned queued_urgent(const sim_net *net, int node)
{
    lmesh_msg_info rows[LMESH_STORE_SIZE];
    size_t n = lmesh_messages(&net->nodes[node].mesh, rows, LMESH_STORE_SIZE);
    unsigned q = 0;
    for (size_t k = 0; k < n; k++) {
        q += rows[k].role == SIM_ROLE_ORIGIN_COPY && rows[k].prio == LMESH_PRIO_URGENT ? 1u : 0u;
    }
    return q;
}

static void top_up(sim_run *r, unsigned load)
{
    for (unsigned q = queued_urgent(&r->net, NODE_A); q < load; q++) {
        if (!submit(r, NODE_A, ID_D, LMESH_MSG_TEXT, LMESH_PRIO_URGENT, LONG_LIFETIME_S, "S11 urgent")) {
            return;
        }
    }
}

static void scen_s11(sim_run *r)
{
    traffic tr;
    sim_topo t;
    unsigned load = s11_loads[(r->m.seed + 1u) % (sizeof s11_loads / sizeof s11_loads[0])];
    metric_set(&r->m, M_LOAD, load);
    topo_line(&t, 4);
    if (!msg_setup(r, &tr, &t, 0, false)) {
        return;
    }
    uint32_t warm = S11_WARM_MIN + sim_rand(&r->net) % S11_WARM_SPREAD;
    uint32_t h_start = r->obs.msg.handovers[NODE_A];
    uint32_t t0 = r->net.now_ms;
    while (r->m.passed && r->obs.msg.handovers[NODE_A] - h_start < warm && r->net.now_ms - t0 < S11_PHASE_MS) {
        top_up(r, load);
        traffic_step(r, &tr, FAST_STEP_MS);
    }
    top_up(r, load);
    sim_track *rt = submit(r, NODE_A, ID_D, LMESH_MSG_TEXT, LMESH_PRIO_ROUTINE, LONG_LIFETIME_S, "S11 routine");
    if (!rt) {
        return;
    }
    uint32_t h0 = r->obs.msg.handovers[NODE_A];
    uint32_t t1 = r->net.now_ms;
    while (r->m.passed && !rt->tx_seen && r->net.now_ms - t1 < S11_PHASE_MS) {
        top_up(r, load);
        traffic_step(r, &tr, FAST_STEP_MS);
    }
    if (!rt->tx_seen) {
        metrics_fail(&r->m, "routine message not handed over within %lu ms under urgent load %u",
                     (unsigned long)S11_PHASE_MS, load);
        return;
    }
    uint32_t between = rt->handover_index - h0 - 1u;
    metric_set(&r->m, M_HANDOVERS, between);
    if (between > S11_HANDOVER_BOUND) {
        metrics_fail(&r->m, "routine message waited %lu handovers under urgent load %u, bound %u",
                     (unsigned long)between, load, S11_HANDOVER_BOUND);
    }
    uint32_t t2 = r->net.now_ms;
    while (!msgobs_all_terminal(&r->obs.msg) && r->net.now_ms - t2 < S11_DRAIN_MS) {
        traffic_step(r, &tr, TRAFFIC_STEP_MS);
    }
    traffic_step(r, &tr, SAMPLE_MS);
    if (!msgobs_all_terminal(&r->obs.msg)) {
        metrics_fail(&r->m, "messages still open %lu ms after the load stopped", (unsigned long)(r->net.now_ms - t2));
    }
    if (rt->received != 1) {
        metrics_fail(&r->m, "routine message delivered %u times at 4", rt->received);
    }
    final_routes(r, "after traffic");
}

typedef struct relay_cut {
    uint16_t id;
    int sender;
    int relay;
    uint8_t seq;
    bool sent;
    bool killed;
    uint32_t kill_ms;
} relay_cut;

static void s12_hook(void *ctx, int from, int to, const uint8_t *frame, size_t len)
{
    relay_cut *w = (relay_cut *)ctx;
    sim_data_view v;
    if (w->killed) {
        return;
    }
    if (to == SIM_TO_AIR && from == w->sender && sim_parse_data(frame, len, &v) && v.ref.origin == ID_A &&
        v.ref.id == w->id) {
        w->sent = true;
        w->seq = v.seq;
        w->relay = sim_index_of(&cur->net, v.rx);
        return;
    }
    if (to == w->sender && w->sent && from == w->relay && (frame[0] & 0x0Fu) == SIM_FT_HOP_ACK &&
        len >= LMESH_HEADER_LEN + 2u && frame[3] == cur->net.nodes[w->sender].ident.id &&
        frame[LMESH_HEADER_LEN] == w->seq &&
        frame[LMESH_HEADER_LEN + 1] == SIM_ACK_ACCEPTED) {
        sim_kill(&cur->net, w->relay);
        w->killed = true;
        w->kill_ms = cur->net.now_ms;
    }
}

static void scen_s12(sim_run *r)
{
    static relay_cut w;
    traffic tr;
    sim_topo t;
    topo_diamond(&t);
    if (!msg_setup(r, &tr, &t, 0, false)) {
        return;
    }
    memset(&w, 0, sizeof w);
    w.sender = NODE_A;
    w.relay = -1;
    r->obs.user_hook = s12_hook;
    r->obs.user_ctx = &w;
    sim_track *tk = submit(r, NODE_A, ID_D, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, LONG_LIFETIME_S, "S12 text");
    if (!tk) {
        return;
    }
    w.id = tk->ref.id;
    uint32_t t0 = r->net.now_ms;
    while (r->net.now_ms - t0 < S12_RUN_MS && !tk->terminal) {
        traffic_step(r, &tr, TRAFFIC_STEP_MS);
    }
    traffic_step(r, &tr, SAMPLE_MS);
    if (!w.killed) {
        metrics_fail(&r->m, "no relay acknowledged the message");
        return;
    }
    if (r->verbose) {
        printf("killed %u at t=%lu ms on its hop ACK\n", r->net.nodes[w.relay].ident.id, (unsigned long)w.kill_ms);
    }
    require_once(r, tk);
    if (tk->received) {
        metric_set(&r->m, M_KILL_TO_DELIVERY_MS, tk->received_ms - w.kill_ms);
        if (tk->rx_hops != 2) {
            metrics_fail(&r->m, "delivered at 4 with hop count %u, expected 2", tk->rx_hops);
        }
    }
    if (tk->resend_frames == 0) {
        metrics_fail(&r->m, "no DATA frame from 1 carried the RESEND bit");
    }
    bool resend_state = false;
    for (uint8_t k = 0; k < tk->n_states; k++) {
        resend_state = resend_state || (tk->states[k] == LMESH_STATE_QUEUED && tk->reasons[k] == LMESH_REASON_RESEND);
    }
    if (!resend_state) {
        metrics_fail(&r->m, "no Queued/resend event at 1");
    }
}

static void scen_s12_line(sim_run *r)
{
    static relay_cut w;
    traffic tr;
    sim_topo t;
    topo_line(&t, 4);
    if (!msg_setup(r, &tr, &t, 0, false)) {
        return;
    }
    memset(&w, 0, sizeof w);
    w.sender = NODE_B;
    w.relay = -1;
    r->obs.user_hook = s12_hook;
    r->obs.user_ctx = &w;
    sim_track *tk = submit(r, NODE_A, ID_D, LMESH_MSG_TEXT, LMESH_PRIO_NORMAL, LONG_LIFETIME_S, "S12L text");
    if (!tk) {
        return;
    }
    w.id = tk->ref.id;
    uint32_t t0 = r->net.now_ms;
    while (r->net.now_ms - t0 < S12_RUN_MS && !tk->terminal) {
        if (w.killed && !r->net.nodes[NODE_C].alive && r->net.now_ms - w.kill_ms >= S12L_DEAD_MS) {
            sim_revive(&r->net, NODE_C);
        }
        traffic_step(r, &tr, TRAFFIC_STEP_MS);
    }
    traffic_step(r, &tr, SAMPLE_MS);
    if (!w.killed || w.relay != NODE_C) {
        metrics_fail(&r->m, "3 never acknowledged the message from 2");
        return;
    }
    require_once(r, tk);
    if (tk->received) {
        metric_set(&r->m, M_KILL_TO_DELIVERY_MS, tk->received_ms - w.kill_ms);
    }
    if (tk->resend_frames == 0) {
        metrics_fail(&r->m, "no DATA frame from 1 carried the RESEND bit");
    }
}

typedef struct s14_counts {
    uint32_t submits;
    uint32_t refused;
    uint32_t evicted[LMESH_PRIO_COUNT];
} s14_counts;

static int evict_rank(const lmesh_msg_info *x, lmesh_prio arriving)
{
    if (x->role == SIM_ROLE_RELAY && x->prio == LMESH_PRIO_ROUTINE) {
        return 0;
    }
    if (x->role == SIM_ROLE_RELAY && x->prio == LMESH_PRIO_NORMAL) {
        return 1;
    }
    if (x->role == SIM_ROLE_ORIGIN_SHADOW && (arriving == LMESH_PRIO_URGENT || x->prio != LMESH_PRIO_URGENT)) {
        return 2;
    }
    if (x->role == SIM_ROLE_ORIGIN_COPY && x->prio > arriving) {
        return 3 + (LMESH_PRIO_ROUTINE - (int)x->prio);
    }
    return -1;
}

static void s14_submit(sim_run *r, traffic *tr, s14_counts *c, lmesh_prio prio, uint32_t lifetime_s)
{
    lmesh_msg_info before[LMESH_STORE_SIZE + 8];
    lmesh_msg_info after[LMESH_STORE_SIZE + 8];
    const lmesh_node *mesh = &r->net.nodes[NODE_A].mesh;
    size_t nb = lmesh_messages(mesh, before, sizeof before / sizeof before[0]);
    int best_rank = -1;
    uint32_t best_left = 0;
    for (size_t k = 0; k < nb && nb >= LMESH_STORE_SIZE; k++) {
        int rank = evict_rank(&before[k], prio);
        if (rank >= 0 && (best_rank < 0 || rank < best_rank || (rank == best_rank && before[k].lifetime_s < best_left))) {
            best_rank = rank;
            best_left = before[k].lifetime_s;
        }
    }
    bool room = nb < LMESH_STORE_SIZE;
    lmesh_msg_ref ref;
    char text[16];
    char p = prio_letter(prio);
    unsigned long no = (unsigned long)++c->submits;
    snprintf(text, sizeof text, "S14 %lu", no);
    lmesh_err e = lmesh_send(&r->net.nodes[NODE_A].mesh, S14_DEST, LMESH_MSG_TEXT, prio, lifetime_s,
                             (const uint8_t *)text, strlen(text), &ref);
    traffic_step(r, tr, FINE_SAMPLE_MS);
    size_t na = lmesh_messages(mesh, after, sizeof after / sizeof after[0]);
    if (na > LMESH_STORE_SIZE) {
        metrics_fail(&r->m, "lmesh_messages lists %lu records", (unsigned long)na);
    }
    const lmesh_msg_info *gone = NULL;
    int n_gone = 0;
    for (size_t k = 0; k < nb; k++) {
        bool still = false;
        for (size_t q = 0; q < na && !still; q++) {
            still = before[k].ref.origin == after[q].ref.origin && before[k].ref.id == after[q].ref.id;
        }
        if (!still) {
            gone = &before[k];
            n_gone++;
        }
    }
    if (!room && best_rank < 0) {
        if (e != LMESH_ERR_FULL || n_gone != 0) {
            metrics_fail(&r->m, "%c submit %lu with nothing evictable returned %d and removed %d records", p, no,
                         (int)e, n_gone);
        }
        c->refused++;
        return;
    }
    if (e != LMESH_OK) {
        metrics_fail(&r->m, "%c submit %lu refused with %d although %s", p, no, (int)e,
                     room ? "the store has room" : "a record is evictable");
        return;
    }
    if (room) {
        if (n_gone != 0) {
            metrics_fail(&r->m, "%c submit %lu removed %d records with room in the store", p, no, n_gone);
        }
        return;
    }
    if (n_gone != 1) {
        metrics_fail(&r->m, "%c submit %lu removed %d records, expected one eviction", p, no, n_gone);
        return;
    }
    if (evict_rank(gone, prio) != best_rank || gone->lifetime_s != best_left) {
        metrics_fail(&r->m, "%c submit %lu evicted %u/%u (role %u prio %u, %lu s left), expected rank %d with %lu s left",
                     p, no, gone->ref.origin, gone->ref.id, gone->role, gone->prio, (unsigned long)gone->lifetime_s,
                     best_rank, (unsigned long)best_left);
    }
    const sim_track *t = msgobs_find(&r->obs.msg, gone->ref);
    if (!t || !t->terminal || t->final_state != LMESH_STATE_FAILED || t->final_reason != LMESH_REASON_EVICTED) {
        metrics_fail(&r->m, "evicted %u/%u not marked Failed/evicted", gone->ref.origin, gone->ref.id);
    }
    if (gone->prio < LMESH_PRIO_COUNT) {
        c->evicted[gone->prio]++;
    }
}

static void s14_burst(sim_run *r, traffic *tr, s14_counts *c, lmesh_prio prio, uint32_t n, uint32_t base_s,
                      uint32_t step_s)
{
    uint32_t perm[LMESH_STORE_SIZE];
    for (uint32_t k = 0; k < n; k++) {
        perm[k] = k;
    }
    for (uint32_t k = n; k-- > 1;) {
        uint32_t j = sim_rand(&r->net) % (k + 1u);
        uint32_t x = perm[k];
        perm[k] = perm[j];
        perm[j] = x;
    }
    for (uint32_t k = 0; k < n; k++) {
        s14_submit(r, tr, c, prio, base_s + step_s * perm[k]);
    }
}

static void scen_s14(sim_run *r)
{
    traffic tr;
    sim_topo t;
    s14_counts c;
    memset(&c, 0, sizeof c);
    topo_line(&t, 1);
    build(r, &t);
    sampler_init(&tr.s);
    sim_step(&r->net, SAMPLE_MS);
    audit_start(r);
    traffic_audit(r, &tr);

    s14_burst(r, &tr, &c, LMESH_PRIO_ROUTINE, LMESH_STORE_SIZE, 2000, 7);
    s14_submit(r, &tr, &c, LMESH_PRIO_ROUTINE, 1000);
    s14_burst(r, &tr, &c, LMESH_PRIO_NORMAL, 8, 3000, 11);
    s14_submit(r, &tr, &c, LMESH_PRIO_ROUTINE, 1000);
    s14_burst(r, &tr, &c, LMESH_PRIO_URGENT, LMESH_STORE_SIZE - 8 + 3, 1500, 5);
    s14_submit(r, &tr, &c, LMESH_PRIO_NORMAL, 1000);
    s14_submit(r, &tr, &c, LMESH_PRIO_ROUTINE, 1000);
    s14_burst(r, &tr, &c, LMESH_PRIO_URGENT, 5, 1200, 13);
    s14_submit(r, &tr, &c, LMESH_PRIO_URGENT, 1000);
    s14_submit(r, &tr, &c, LMESH_PRIO_NORMAL, 1000);
    s14_submit(r, &tr, &c, LMESH_PRIO_ROUTINE, 1000);
    traffic_step(r, &tr, SAMPLE_MS);

    uint32_t observed = c.evicted[0] + c.evicted[1] + c.evicted[2];
    lmesh_stats st;
    lmesh_get_stats(&r->net.nodes[NODE_A].mesh, &st);
    const sim_audit *a = &r->obs.msg.audit;
    uint32_t failed_evicted = 0;
    for (uint32_t k = 0; k < r->obs.msg.count; k++) {
        const sim_track *tk = &r->obs.msg.t[k];
        if (tk->terminal && tk->final_reason == LMESH_REASON_EVICTED) {
            failed_evicted++;
            if (tk->prio == LMESH_PRIO_URGENT) {
                metrics_fail(&r->m, "urgent record %u/%u was evicted", tk->ref.origin, tk->ref.id);
            }
        }
    }
    metric_set(&r->m, M_REFUSED_SUBMITS, c.refused);
    metric_set(&r->m, M_EVICTED_ROUTINE, c.evicted[LMESH_PRIO_ROUTINE]);
    metric_set(&r->m, M_EVICTED_NORMAL, c.evicted[LMESH_PRIO_NORMAL]);
    metric_set(&r->m, M_EVICTED_URGENT, c.evicted[LMESH_PRIO_URGENT]);
    if (c.evicted[LMESH_PRIO_ROUTINE] != LMESH_STORE_SIZE || c.evicted[LMESH_PRIO_NORMAL] != 8 ||
        c.evicted[LMESH_PRIO_URGENT] != 0) {
        metrics_fail(&r->m, "evictions routine=%lu normal=%lu urgent=%lu, expected %d, 8, 0",
                     (unsigned long)c.evicted[2], (unsigned long)c.evicted[1], (unsigned long)c.evicted[0],
                     LMESH_STORE_SIZE);
    }
    if (c.refused != 7) {
        metrics_fail(&r->m, "%lu submits refused, expected 7", (unsigned long)c.refused);
    }
    if (st.evicted != observed || failed_evicted != observed || a->store_ev_evicted[NODE_A] != observed) {
        metrics_fail(&r->m, "observed %lu evictions; stats %lu, Failed/evicted events %lu, last store event %u",
                     (unsigned long)observed, (unsigned long)st.evicted, (unsigned long)failed_evicted,
                     a->store_ev_evicted[NODE_A]);
    }
}

static const sim_scenario scenarios[] = {
    {"smoke", scen_smoke}, {"S1", scen_s1},   {"S2", scen_s2},   {"S3", scen_s3},   {"S4", scen_s4},
    {"S5", scen_s5},       {"S6", scen_s6},   {"S7", scen_s7},   {"S8", scen_s8},   {"S9", scen_s9},
    {"S10", scen_s10},     {"S11", scen_s11}, {"S12", scen_s12}, {"S13", scen_s13}, {"S14", scen_s14},
    {"S12L", scen_s12_line},
};

const sim_scenario *sim_find_scenario(const char *name)
{
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        if (strcmp(scenarios[i].name, name) == 0) {
            return &scenarios[i];
        }
    }
    return NULL;
}

void sim_list_scenarios(FILE *out)
{
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        fprintf(out, "%s%s%s", i ? " " : "", scenarios[i].name, scenarios[i].fn ? "" : "(stub)");
    }
    fprintf(out, "\n");
}

void sim_run_scenario(sim_run *run, const sim_scenario *sc, uint32_t seed, bool verbose)
{
    cur = run;
    check_reset();
    sim_init(&run->net, seed);
    obs_init(&run->obs, &run->net, verbose);
    metrics_init(&run->m, sc->name, seed);
    memset(&run->topo, 0, sizeof run->topo);
    run->verbose = verbose;
    sc->fn(run);
    if (run->obs.msg.audit.samples > 0) {
        msgobs_sample(&run->obs.msg, &run->net);
    }
    metrics_collect(&run->m, &run->net, &run->obs);
}
