/* Simulator invariants and metrics: route-graph walks, convergence tracking, event and frame observation, CSV output. */
#ifndef LMESH_SIM_CHECK_H
#define LMESH_SIM_CHECK_H

#include <stdio.h>

#include "sim_msg.h"
#include "sim_net.h"
#include "sim_topo.h"

#define SIM_MSG_LOG 512
#define SIM_EVENT_TYPES (LMESH_EV_STORE + 1)
#define SIM_FT_HELLO 0
#define SIM_FT_DATA 1
#define SIM_FT_HOP_ACK 2
#define SIM_FT_STATUS 3
#define SIM_FT_ROUTE_REQ 4

typedef struct sim_obs {
    sim_net *net;
    bool verbose;
    uint32_t events[SIM_MAX_NODES][SIM_EVENT_TYPES];
    uint16_t route_lost[SIM_MAX_NODES][SIM_MAX_NODES];
    uint16_t route_changed[SIM_MAX_NODES][SIM_MAX_NODES];
    lmesh_node_id last_hop[SIM_MAX_NODES][SIM_MAX_NODES];
    uint16_t msg_count[SIM_MAX_NODES];
    lmesh_msg_ref msgs[SIM_MAX_NODES][SIM_MSG_LOG];
    uint32_t msg_duplicates[SIM_MAX_NODES];
    uint32_t msg_overflow;
    uint32_t frames_tx[SIM_FRAME_TYPES];
    uint32_t frames_rx[SIM_FRAME_TYPES];
    uint32_t req_originated;
    uint8_t data_hops_max;
    uint32_t hop_violations;
    sim_msgobs msg;
    sim_frame_hook user_hook;
    void *user_ctx;
} sim_obs;

void obs_init(sim_obs *o, sim_net *net, bool verbose);
void obs_event(sim_obs *o, int node, const lmesh_event *ev);
void obs_clear_route_events(sim_obs *o);
uint32_t obs_duplicates(const sim_obs *o);
bool obs_received(const sim_obs *o, int node, lmesh_msg_ref ref);

typedef struct sim_route_report {
    int pairs;
    int ok;
    int missing;
    int loops;
    int incomplete;
    int phantom;
    int metric_checked;
    int metric_low;
    int metric_mismatch;
    int metric_perturbed;
    int nonshortest;
    int hop_checked;
    int hop_mismatch;
    char first[192];
    char loop_first[192];
} sim_route_report;

void check_reset(void);
bool sim_route_get(const sim_net *net, int src, int dest, lmesh_route_info *out);
void check_routes(const sim_net *net, bool check_metrics, sim_route_report *rep);
bool report_converged(const sim_route_report *rep);
uint32_t sim_clean_window_ms(int hops);

typedef struct sim_tracker {
    bool ok;
    bool ever;
    uint32_t since_ms;
    uint32_t first_ms;
} sim_tracker;

void tracker_reset(sim_tracker *t);
void tracker_sample(sim_tracker *t, bool ok, uint32_t now_ms);

typedef enum sim_metric_id {
    M_NODES = 0,
    M_LINKS,
    M_DIAMETER,
    M_DIAMETER_AFTER,
    M_DISCOVERY_MS,
    M_ETX_SETTLE_MS,
    M_FIRST_ROUTE_MS,
    M_FIRST_CONVERGENCE_MS,
    M_CONVERGENCE_MS,
    M_REROUTE_MS,
    M_RECOVERY_MS,
    M_RETRACT_MS,
    M_STALE_DELAY_MS,
    M_STALE_DELAY_MIN_MS,
    M_FALSE_STALE,
    M_ROUTE_METRIC8,
    M_ROUTE_REQUESTS,
    M_ROUTE_REQUESTS_AFTER,
    M_REQ_ORIGINATED_AFTER,
    M_ROUTE_LOST_EVENTS,
    M_ROUTE_CHANGED_EVENTS,
    M_SRC_ROUTE_LOST,
    M_SRC_ROUTE_CHANGED,
    M_HELLO_FRAMES,
    M_STATUS_FRAMES,
    M_DATA_FRAMES,
    M_AIRTIME_MS,
    M_HALF_DUPLEX_DROPS,
    M_LOOPS,
    M_LOOP_SAMPLES,
    M_METRIC_LOW,
    M_METRIC_MISMATCH,
    M_METRIC_PERTURBED,
    M_METRIC_SUM_CHECKED,
    M_METRIC_SUM_MISMATCH,
    M_NONSHORTEST,
    M_PHANTOM_ROUTES,
    M_HOP_VIOLATIONS,
    M_DELIVERY_RATIO,
    M_LATENCY_MEAN_MS,
    M_LATENCY_MAX_MS,
    M_DUPLICATES,
    M_STORE_DEPTH_MAX,
    M_SIM_TIME_MS,
    M_MSGS_ORIGINATED,
    M_MSGS_RECEIVED,
    M_MSGS_DELIVERED,
    M_MSGS_EXPIRED,
    M_MSGS_FAILED,
    M_MSGS_OPEN,
    M_LATENCY_P50_MS,
    M_LATENCY_P95_MS,
    M_RX_LATENCY_MEAN_MS,
    M_RX_LATENCY_P50_MS,
    M_RX_LATENCY_P95_MS,
    M_RX_LATENCY_MAX_MS,
    M_STORE_DEPTH_FINAL,
    M_RECEIPTS_SENT,
    M_DATA_FIRST_TX,
    M_DATA_RETRIES,
    M_RESEND_FRAMES,
    M_EVICTIONS,
    M_EXPIRIES,
    M_TERMINAL_VIOLATIONS,
    M_STORE_CUSTODY,
    M_STORE_BY_COUNTER,
    M_STORE_ORIGIN_TERMINAL,
    M_STORE_REFUSED,
    M_STORE_DEATH_LOST,
    M_STORE_UNEXPLAINED,
    M_STORE_LISTED_MAX,
    M_LOAD,
    M_HANDOVERS,
    M_CMD_EXECUTIONS,
    M_HOLD_DATA_FRAMES,
    M_EXPIRE_AFTER_MS,
    M_HEAL_FIRST_MS,
    M_HEAL_LAST_MS,
    M_KILL_TO_DELIVERY_MS,
    M_REFUSED_SUBMITS,
    M_EVICTED_ROUTINE,
    M_EVICTED_NORMAL,
    M_EVICTED_URGENT,
    M_COUNT
} sim_metric_id;

typedef struct sim_metrics {
    const char *scenario;
    uint32_t seed;
    bool passed;
    int failures;
    bool set[M_COUNT];
    double v[M_COUNT];
    char fail[192];
    char depth_max_by_node[64];
    char depth_final_by_node[64];
} sim_metrics;

void metrics_init(sim_metrics *m, const char *scenario, uint32_t seed);
void metric_set(sim_metrics *m, sim_metric_id id, double value);
void metric_max(sim_metrics *m, sim_metric_id id, double value);
void metrics_fail(sim_metrics *m, const char *fmt, ...);
void metrics_collect(sim_metrics *m, const sim_net *net, const sim_obs *o);
void metrics_print(const sim_metrics *m);
void metrics_summary(const sim_metrics *m);
int metrics_csv_append(const sim_metrics *m, const char *path);
void msgobs_metrics(const sim_msgobs *mo, const sim_net *net, sim_metrics *m);
int msgobs_csv_append(const sim_msgobs *mo, const sim_metrics *m, const char *csv);
const char *metric_name(sim_metric_id id);
bool metric_lower_is_worse(sim_metric_id id);
void metric_format(double v, char *out, size_t out_max);

#endif
