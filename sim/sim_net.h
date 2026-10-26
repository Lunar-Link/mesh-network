/* Host simulator: N library instances on a virtual medium with per-link loss, node kill and revive, and a stepped clock. */
#ifndef LMESH_SIM_NET_H
#define LMESH_SIM_NET_H

#include "lmesh/lmesh.h"

#define SIM_MAX_NODES 16
#define SIM_FRAME_TYPES 16
#define SIM_TO_AIR (-1)

struct sim_net;

typedef void (*sim_frame_hook)(void *ctx, int from, int to, const uint8_t *frame, size_t len);
typedef void (*sim_iter_hook)(void *ctx, struct sim_net *net);

typedef struct sim_port_ctx {
    struct sim_net *net;
    int index;
} sim_port_ctx;

typedef struct sim_node {
    lmesh_node mesh;
    sim_port_ctx ctx;
    lmesh_identity ident;
    bool alive;
    bool tx_active;
    uint32_t tx_done_ms;
    uint32_t alive_since_ms;
    size_t tx_len;
    uint8_t tx_buf[LMESH_MAX_FRAME];
    uint32_t frames_tx;
    uint32_t frames_rx;
} sim_node;

typedef struct sim_net {
    uint32_t now_ms;
    uint32_t rng;
    int count;
    sim_node nodes[SIM_MAX_NODES];
    bool link_up[SIM_MAX_NODES][SIM_MAX_NODES];
    uint8_t loss_pct[SIM_MAX_NODES][SIM_MAX_NODES];
    uint32_t link_since_ms[SIM_MAX_NODES][SIM_MAX_NODES];
    bool dropped[SIM_MAX_NODES][SIM_MAX_NODES];
    uint32_t last_drop_ms[SIM_MAX_NODES][SIM_MAX_NODES];
    uint32_t airtime_fixed_ms;
    uint32_t airtime_us_per_byte;
    uint32_t frames_sent;
    uint32_t frames_delivered;
    uint32_t frames_lost;
    uint32_t frames_half_duplex;
    sim_frame_hook on_frame;
    void *frame_ctx;
    sim_iter_hook on_iter;
    void *iter_ctx;
} sim_net;

void sim_init(sim_net *net, uint32_t seed);
int sim_add_node(sim_net *net, lmesh_node_id id, uint8_t network_id, void (*on_event)(void *ctx, const lmesh_event *ev),
                 void (*on_command)(void *ctx, lmesh_node_id from, const uint8_t *data, size_t len,
                                    lmesh_cmd_result *result));
void sim_set_link(sim_net *net, int a, int b, bool up, uint8_t loss_pct);
void sim_set_loss_dir(sim_net *net, int from, int to, uint8_t loss_pct);
void sim_kill(sim_net *net, int i);
void sim_revive(sim_net *net, int i);
void sim_step(sim_net *net, uint32_t ms);
uint32_t sim_rand(sim_net *net);
int sim_index_of(const sim_net *net, lmesh_node_id id);
bool sim_linked(const sim_net *net, int a, int b);
bool sim_link_clean(const sim_net *net, int a, int b, uint32_t window_ms);

#endif
