/* Terrain medium: link tables from terrain-sims/models/linktable.py, model-error draws, and per-step loss, RSSI and SNR on the medium. */
#ifndef LMESH_SIM_TERRAIN_H
#define LMESH_SIM_TERRAIN_H

#include <stddef.h>

#include "sim_net.h"

#define SIM_TERRAIN_POOL (1u << 20)
#define SIM_TERRAIN_KNOTS 512
#define SIM_TERRAIN_NAME 64
#define SIM_TERRAIN_NODE_NAME 16

typedef struct sim_terrain_node {
    char name[SIM_TERRAIN_NODE_NAME];
    double start_s;
    bool mobile;
    uint32_t xy_off;
    uint32_t xy_n;
    uint32_t dist_off;
    uint32_t dist_n;
} sim_terrain_node;

typedef struct sim_terrain {
    bool loaded;
    char name[SIM_TERRAIN_NAME];
    char preset[SIM_TERRAIN_NODE_NAME];
    int n;
    uint32_t times;
    double step_s;
    double tx_dbm;
    double sens_dbm;
    double noise_dbm;
    double per_ref;
    double per_width_db;
    sim_lora lora;
    sim_terrain_node node[SIM_MAX_NODES];
    uint32_t link_off[SIM_MAX_NODES][SIM_MAX_NODES];
    uint32_t link_n[SIM_MAX_NODES][SIM_MAX_NODES];
    uint32_t used;
    float pool[SIM_TERRAIN_POOL];
    double sigma_db;
    double bias_db;
    double decor_m;
    double bias;
    float knot[SIM_MAX_NODES][SIM_MAX_NODES][SIM_TERRAIN_KNOTS];
} sim_terrain;

bool terrain_load(sim_terrain *t, const char *path, char *err, size_t err_max);
void terrain_draw(sim_terrain *t, uint32_t seed, double sigma_db, double bias_db, double decor_m);
double terrain_end_s(const sim_terrain *t);
double terrain_loss_db(const sim_terrain *t, int a, int b, double t_s);
double terrain_error_db(const sim_terrain *t, int a, int b, double t_s);
double terrain_margin_db(const sim_terrain *t, int a, int b, double t_s);
double terrain_per(const sim_terrain *t, double margin_db);
void terrain_position(const sim_terrain *t, int i, double t_s, double *x, double *y);
void terrain_apply(const sim_terrain *t, sim_net *net, double t_s);
bool terrain_connected(const sim_terrain *t, const sim_net *net, int a, int b, double t_s, double min_margin_db);

#endif
