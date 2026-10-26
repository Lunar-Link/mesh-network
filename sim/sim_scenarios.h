/* Simulator scenarios from PLAN.md Section 5 and the registry the runner looks them up in. */
#ifndef LMESH_SIM_SCENARIOS_H
#define LMESH_SIM_SCENARIOS_H

#include "sim_check.h"

typedef struct sim_run {
    sim_net net;
    sim_obs obs;
    sim_metrics m;
    sim_topo topo;
    bool verbose;
} sim_run;

typedef void (*sim_scenario_fn)(sim_run *run);

typedef struct sim_scenario {
    const char *name;
    sim_scenario_fn fn;
} sim_scenario;

const sim_scenario *sim_find_scenario(const char *name);
void sim_list_scenarios(FILE *out);
void sim_run_scenario(sim_run *run, const sim_scenario *sc, uint32_t seed, bool verbose);

#endif
