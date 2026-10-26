/* Scenario runner. Usage: lmesh_sim <scenario> [seed]. Scenarios are listed in PLAN.md Section 5. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_net.h"

static void on_event(void *ctx, const lmesh_event *ev)
{
    const sim_node *sn = (const sim_node *)ctx;
    char line[256];
    size_t n = lmesh_host_format_event(ev, line, sizeof line);
    if (n > 0) {
        printf("node %u %s", sn->ident.id, line);
    }
}

static sim_net net;

static int add(lmesh_node_id id)
{
    return sim_add_node(&net, id, 1, on_event, NULL);
}

static void report(void)
{
    printf("sim: t=%lu ms sent=%lu delivered=%lu lost=%lu half_duplex=%lu\n", (unsigned long)net.now_ms,
           (unsigned long)net.frames_sent, (unsigned long)net.frames_delivered, (unsigned long)net.frames_lost,
           (unsigned long)net.frames_half_duplex);
    for (int i = 0; i < net.count; i++) {
        lmesh_stats st;
        lmesh_get_stats(&net.nodes[i].mesh, &st);
        printf("node %u: alive=%d tx=%lu rx=%lu malformed=%lu\n", net.nodes[i].ident.id, net.nodes[i].alive,
               (unsigned long)net.nodes[i].frames_tx, (unsigned long)net.nodes[i].frames_rx,
               (unsigned long)st.rx_malformed);
    }
}

static int scenario_smoke(void)
{
    int a = add(1), b = add(2), c = add(3), d = add(4);
    sim_set_link(&net, a, b, true, 0);
    sim_set_link(&net, b, c, true, 0);
    sim_set_link(&net, c, d, true, 0);
    sim_step(&net, 60000);
    report();
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "smoke";
    uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1u;

    sim_init(&net, seed);
    if (strcmp(name, "smoke") == 0) {
        return scenario_smoke();
    }
    fprintf(stderr, "unknown scenario: %s\n", name);
    return 2;
}
