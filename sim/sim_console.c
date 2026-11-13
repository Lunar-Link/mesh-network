/* Simulated network with one node's line protocol on stdin and stdout, for driving the interface without boards. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sim_topo.h"
#include "sim_terrain.h"

#define NETWORK_ID 1
#define LINE_MAX 512
#define OUT_MAX 16384

static sim_net net;
static int attached;
static char out[OUT_MAX];
static sim_terrain terrain;
static bool started[SIM_MAX_NODES], manual_off[SIM_MAX_NODES];
static bool cut[SIM_MAX_NODES][SIM_MAX_NODES];
static unsigned extra_loss;

static void apply_terrain(void)
{
    double ts = net.now_ms / 1000.0;
    for (int i = 0; i < net.count; i++) {
        if (!started[i] && ts >= terrain.node[i].start_s) {
            started[i] = true;
            if (!manual_off[i]) {
                sim_revive(&net, i);
            }
        }
    }
    terrain_apply(&terrain, &net, ts);
    for (int a = 0; a < net.count; a++) {
        for (int b = a + 1; b < net.count; b++) {
            for (int d = 0; d < 2; d++) {
                int from = d ? b : a, to = d ? a : b;
                unsigned loss = net.loss_pct[from][to];
                sim_set_loss_dir(&net, from, to, (uint8_t)(100u - ((100u - loss) * (100u - extra_loss) + 50u) / 100u));
            }
            if (cut[a][b]) {
                sim_set_link(&net, a, b, false, 100);
            }
        }
    }
}

static void advance(uint32_t ms)
{
    if (!terrain.loaded) {
        sim_step(&net, ms);
        return;
    }
    while (ms > 0) {
        apply_terrain();
        uint32_t step = ms < 1000u ? ms : 1000u;
        sim_step(&net, step);
        ms -= step;
    }
    apply_terrain();
}

static int usage(void)
{
    fprintf(stderr, "usage: lmesh_console [line|ring|diamond|random] [nodes] [seed] [loss_pct]\n"
                    "       lmesh_console terrain <table> [seed] [extra_loss_pct]\n"
                    "stdin: line protocol commands for the attached node, or\n"
                    "  @t <ms>  @kill <id>  @revive <id>  @cut <a> <b>  @join <a> <b> [loss_pct]  @attach <id>  @topo\n");
    return 2;
}

static void on_event(void *ctx, const lmesh_event *ev)
{
    const sim_node *sn = (const sim_node *)ctx;
    if (sn->ctx.index != attached) {
        return;
    }
    char text[1024];
    size_t n = lmesh_host_format_event(ev, text, sizeof text);
    fwrite(text, 1, n, stdout);
}

static void on_command(void *ctx, lmesh_node_id from, const uint8_t *data, size_t len, lmesh_cmd_result *result)
{
    (void)ctx;
    (void)from;
    (void)data;
    (void)len;
    result->code = LMESH_RESULT_EXECUTED;
    result->len = 2;
    memcpy(result->data, "ok", 2);
}

static void reply(bool ok, const char *reason)
{
    if (ok) {
        puts("{\"ok\":true}");
    } else {
        printf("{\"ok\":false,\"reason\":\"%s\"}\n", reason);
    }
}

static int node_arg(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return -1;
    }
    return sim_index_of(&net, (lmesh_node_id)v);
}

static void print_topo(void)
{
    printf("{\"ok\":true,\"attached\":%d,\"alive\":[", net.nodes[attached].ident.id);
    bool first = true;
    for (int i = 0; i < net.count; i++) {
        if (net.nodes[i].alive) {
            printf(first ? "%d" : ",%d", net.nodes[i].ident.id);
            first = false;
        }
    }
    printf("],\"links\":[");
    first = true;
    for (int a = 0; a < net.count; a++) {
        for (int b = a + 1; b < net.count; b++) {
            if (net.link_up[a][b]) {
                printf(first ? "[%d,%d,%d]" : ",[%d,%d,%d]", net.nodes[a].ident.id, net.nodes[b].ident.id,
                       net.loss_pct[a][b]);
                first = false;
            }
        }
    }
    printf("],\"nodes\":[");
    for (int i = 0; i < net.count; i++) {
        printf(i ? ",%u" : "%u", net.nodes[i].ident.id);
    }
    if (terrain.loaded) {
        printf("],\"terrain\":true,\"margins\":[");
        first = true;
        double ts = net.now_ms / 1000.0;
        for (int a = 0; a < net.count; a++) {
            for (int b = a + 1; b < net.count; b++) {
                double ab = terrain_margin_db(&terrain, a, b, ts), ba = terrain_margin_db(&terrain, b, a, ts);
                printf(first ? "[%u,%u," : ",[%u,%u,", net.nodes[a].ident.id, net.nodes[b].ident.id);
                if (isfinite(ab) && isfinite(ba)) {
                    printf("%.3f,%.3f,", ab, ba);
                } else {
                    printf("null,null,");
                }
                printf("%s]", cut[a][b] || !net.nodes[a].alive || !net.nodes[b].alive ? "true" : "false");
                first = false;
            }
        }
    }
    puts("]}");
}

static void control(char *line)
{
    char *argv[4] = {NULL, NULL, NULL, NULL};
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 4; tok = strtok(NULL, " ")) {
        argv[argc++] = tok;
    }
    const char *cmd = argv[0];
    int a = argc > 1 ? node_arg(argv[1]) : -1;
    int b = argc > 2 ? node_arg(argv[2]) : -1;

    if (strcmp(cmd, "@t") == 0) {
        long ms = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
        if (ms > 0 && ms <= 3600000L) {
            advance((uint32_t)ms);
        }
    } else if (strcmp(cmd, "@topo") == 0) {
        print_topo();
    } else if (strcmp(cmd, "@loss") == 0 && terrain.loaded) {
        char *end = NULL;
        long loss = argc == 2 ? strtol(argv[1], &end, 10) : -1;
        if (loss < 0 || loss > 100 || !end || end == argv[1] || *end != '\0') {
            reply(false, "bad_loss");
        } else {
            extra_loss = (unsigned)loss;
            apply_terrain();
            reply(true, NULL);
        }
    } else if (a < 0) {
        reply(false, "bad_node");
    } else if (strcmp(cmd, "@kill") == 0) {
        manual_off[a] = true;
        sim_kill(&net, a);
        reply(true, NULL);
    } else if (strcmp(cmd, "@revive") == 0) {
        manual_off[a] = false;
        started[a] = true;
        if (!net.nodes[a].alive) {
            sim_revive(&net, a);
        }
        reply(true, NULL);
    } else if (strcmp(cmd, "@attach") == 0) {
        attached = a;
        reply(true, NULL);
    } else if (b < 0 || a == b) {
        reply(false, "bad_node");
    } else if (strcmp(cmd, "@cut") == 0) {
        cut[a][b] = cut[b][a] = true;
        sim_set_link(&net, a, b, false, 0);
        reply(true, NULL);
    } else if (strcmp(cmd, "@join") == 0) {
        cut[a][b] = cut[b][a] = false;
        if (terrain.loaded) {
            apply_terrain();
        } else {
            long loss = argc > 3 ? strtol(argv[3], NULL, 10) : 0;
            sim_set_link(&net, a, b, true, (uint8_t)(loss < 0 ? 0 : loss > 100 ? 100 : loss));
        }
        reply(true, NULL);
    } else {
        reply(false, "unknown_command");
    }
}

int main(int argc, char **argv)
{
    const char *shape = argc > 1 ? argv[1] : "random";
    bool use_terrain = strcmp(shape, "terrain") == 0;
    int n = use_terrain ? 2 : argc > 2 ? atoi(argv[2]) : 8;
    uint32_t seed = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 1u;
    int loss = argc > 4 ? atoi(argv[4]) : 0;
    if (n < 2 || n > SIM_MAX_NODES || loss < 0 || loss > 100) {
        return usage();
    }

    sim_init(&net, seed);
    sim_topo t;
    if (use_terrain) {
        char error[256];
        if (argc < 3 || !terrain_load(&terrain, argv[2], error, sizeof error)) {
            fprintf(stderr, "%s\n", argc < 3 ? "missing terrain table" : error);
            return 2;
        }
        terrain_draw(&terrain, seed, 0, 0, 100);
        net.lora = terrain.lora;
        extra_loss = (unsigned)loss;
        topo_clear(&t, terrain.n);
    } else if (strcmp(shape, "line") == 0) {
        topo_line(&t, n);
    } else if (strcmp(shape, "ring") == 0) {
        topo_ring(&t, n);
    } else if (strcmp(shape, "diamond") == 0) {
        topo_diamond(&t);
    } else if (strcmp(shape, "random") == 0) {
        topo_random(&t, n, n / 2, &net);
    } else {
        return usage();
    }
    for (int i = 0; i < t.n; i++) {
        if (sim_add_node(&net, (lmesh_node_id)(i + 1), NETWORK_ID, on_event, on_command) < 0) {
            fprintf(stderr, "cannot add node %d\n", i + 1);
            return 1;
        }
        sim_node *sn = &net.nodes[i];
        int len = snprintf(sn->ident.station_id, LMESH_STATION_ID_MAX, "SIM%d", i + 1);
        sn->ident.station_id_len = (uint8_t)len;
        sn->mesh.ident = sn->ident;
    }
    if (terrain.loaded) {
        for (int i = 0; i < net.count; i++) {
            started[i] = terrain.node[i].start_s <= 0;
            if (!started[i]) {
                sim_kill(&net, i);
            }
        }
        apply_terrain();
    } else {
        topo_apply(&net, &t, (uint8_t)loss);
    }
    fflush(stdout);

    char line[LINE_MAX];
    while (fgets(line, sizeof line, stdin)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (len == 0) {
            continue;
        }
        if (line[0] == '@') {
            control(line);
        } else if (net.nodes[attached].alive) {
            size_t k = lmesh_host_command(&net.nodes[attached].mesh, line, out, sizeof out);
            fwrite(out, 1, k, stdout);
        } else {
            reply(false, "node_down");
        }
        fflush(stdout);
    }
    return 0;
}
