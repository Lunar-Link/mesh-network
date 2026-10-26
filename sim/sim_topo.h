/* Simulator topologies: line, diamond, ring with chords, random connected graphs, BFS distances, kill candidates. */
#ifndef LMESH_SIM_TOPO_H
#define LMESH_SIM_TOPO_H

#include "sim_net.h"

#define SIM_UNREACHABLE (-1)

typedef struct sim_topo {
    int n;
    bool adj[SIM_MAX_NODES][SIM_MAX_NODES];
} sim_topo;

void topo_clear(sim_topo *t, int n);
void topo_edge(sim_topo *t, int a, int b);
int topo_edge_count(const sim_topo *t);
void topo_line(sim_topo *t, int n);
void topo_diamond(sim_topo *t);
void topo_ring(sim_topo *t, int n);
void topo_random(sim_topo *t, int n, int extra, sim_net *net);
void topo_apply(sim_net *net, const sim_topo *t, uint8_t loss_pct);
void topo_describe(const sim_topo *t, char *out, size_t out_max);

void sim_bfs(const sim_net *net, int src, int skip, int dist[SIM_MAX_NODES]);
int sim_diameter(const sim_net *net);
bool sim_alive_connected(const sim_net *net, int skip);
int sim_kill_candidate(sim_net *net, bool prefer_relay);

#endif
