/* Simulator topologies and graph queries over the live medium state. */
#include "sim_topo.h"

#include <stdio.h>
#include <string.h>

void topo_clear(sim_topo *t, int n)
{
    memset(t, 0, sizeof *t);
    t->n = n;
}

void topo_edge(sim_topo *t, int a, int b)
{
    if (a != b) {
        t->adj[a][b] = true;
        t->adj[b][a] = true;
    }
}

int topo_edge_count(const sim_topo *t)
{
    int e = 0;
    for (int a = 0; a < t->n; a++) {
        for (int b = a + 1; b < t->n; b++) {
            e += t->adj[a][b] ? 1 : 0;
        }
    }
    return e;
}

void topo_line(sim_topo *t, int n)
{
    topo_clear(t, n);
    for (int i = 0; i + 1 < n; i++) {
        topo_edge(t, i, i + 1);
    }
}

void topo_diamond(sim_topo *t)
{
    topo_clear(t, 4);
    topo_edge(t, 0, 1);
    topo_edge(t, 1, 3);
    topo_edge(t, 0, 2);
    topo_edge(t, 2, 3);
}

void topo_ring(sim_topo *t, int n)
{
    topo_clear(t, n);
    for (int i = 0; i < n; i++) {
        topo_edge(t, i, (i + 1) % n);
    }
}

void topo_random(sim_topo *t, int n, int extra, sim_net *net)
{
    int order[SIM_MAX_NODES];
    topo_clear(t, n);
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(sim_rand(net) % (uint32_t)(i + 1));
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (int i = 1; i < n; i++) {
        topo_edge(t, order[i], order[sim_rand(net) % (uint32_t)i]);
    }
    int max_edges = n * (n - 1) / 2;
    for (int k = 0, tries = 0; k < extra && topo_edge_count(t) < max_edges && tries < 1000; tries++) {
        int a = (int)(sim_rand(net) % (uint32_t)n);
        int b = (int)(sim_rand(net) % (uint32_t)n);
        if (a != b && !t->adj[a][b]) {
            topo_edge(t, a, b);
            k++;
        }
    }
}

void topo_apply(sim_net *net, const sim_topo *t, uint8_t loss_pct)
{
    for (int a = 0; a < t->n; a++) {
        for (int b = a + 1; b < t->n; b++) {
            sim_set_link(net, a, b, t->adj[a][b], t->adj[a][b] ? loss_pct : 0);
        }
    }
}

void topo_describe(const sim_topo *t, char *out, size_t out_max)
{
    size_t pos = 0;
    if (out_max == 0) {
        return;
    }
    out[0] = '\0';
    for (int a = 0; a < t->n; a++) {
        for (int b = a + 1; b < t->n; b++) {
            if (t->adj[a][b] && pos + 12 < out_max) {
                int w = snprintf(out + pos, out_max - pos, "%s%d-%d", pos ? " " : "", a + 1, b + 1);
                pos += w > 0 ? (size_t)w : 0u;
            }
        }
    }
}

void sim_bfs(const sim_net *net, int src, int skip, int dist[SIM_MAX_NODES])
{
    int queue[SIM_MAX_NODES];
    int head = 0;
    int tail = 0;
    for (int i = 0; i < SIM_MAX_NODES; i++) {
        dist[i] = SIM_UNREACHABLE;
    }
    if (src < 0 || src >= net->count || src == skip || !net->nodes[src].alive) {
        return;
    }
    dist[src] = 0;
    queue[tail++] = src;
    while (head < tail) {
        int u = queue[head++];
        for (int v = 0; v < net->count; v++) {
            if (v != skip && dist[v] == SIM_UNREACHABLE && sim_linked(net, u, v)) {
                dist[v] = dist[u] + 1;
                queue[tail++] = v;
            }
        }
    }
}

int sim_diameter(const sim_net *net)
{
    int worst = 0;
    int dist[SIM_MAX_NODES];
    for (int s = 0; s < net->count; s++) {
        sim_bfs(net, s, -1, dist);
        for (int d = 0; d < net->count; d++) {
            worst = dist[d] > worst ? dist[d] : worst;
        }
    }
    return worst;
}

bool sim_alive_connected(const sim_net *net, int skip)
{
    int first = -1;
    for (int i = 0; i < net->count && first < 0; i++) {
        if (i != skip && net->nodes[i].alive) {
            first = i;
        }
    }
    if (first < 0) {
        return true;
    }
    int dist[SIM_MAX_NODES];
    sim_bfs(net, first, skip, dist);
    for (int i = 0; i < net->count; i++) {
        if (i != skip && net->nodes[i].alive && dist[i] == SIM_UNREACHABLE) {
            return false;
        }
    }
    return true;
}

static int live_degree(const sim_net *net, int i)
{
    int d = 0;
    for (int j = 0; j < net->count; j++) {
        d += sim_linked(net, i, j) ? 1 : 0;
    }
    return d;
}

int sim_kill_candidate(sim_net *net, bool prefer_relay)
{
    int cand[SIM_MAX_NODES];
    int relay[SIM_MAX_NODES];
    int n = 0;
    int r = 0;
    int alive = 0;
    for (int i = 0; i < net->count; i++) {
        alive += net->nodes[i].alive ? 1 : 0;
    }
    if (alive < 3) {
        return -1;
    }
    for (int i = 0; i < net->count; i++) {
        if (net->nodes[i].alive && sim_alive_connected(net, i)) {
            cand[n++] = i;
            if (live_degree(net, i) >= 2) {
                relay[r++] = i;
            }
        }
    }
    if (prefer_relay && r > 0) {
        return relay[sim_rand(net) % (uint32_t)r];
    }
    return n ? cand[sim_rand(net) % (uint32_t)n] : -1;
}
