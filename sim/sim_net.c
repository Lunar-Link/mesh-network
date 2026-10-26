/* Host simulator: virtual clock, virtual medium, port implementation for simulated nodes. */
#include "sim_net.h"

#include <string.h>

static uint32_t port_now_ms(void *ctx)
{
    return ((sim_port_ctx *)ctx)->net->now_ms;
}

static uint32_t port_random(void *ctx)
{
    return sim_rand(((sim_port_ctx *)ctx)->net);
}

static uint32_t port_airtime_ms(void *ctx, size_t len)
{
    sim_net *net = ((sim_port_ctx *)ctx)->net;
    return net->airtime_fixed_ms + (uint32_t)((len * net->airtime_us_per_byte + 999u) / 1000u);
}

static int port_radio_send(void *ctx, const uint8_t *frame, size_t len)
{
    sim_port_ctx *pc = (sim_port_ctx *)ctx;
    sim_node *sn = &pc->net->nodes[pc->index];
    if (sn->tx_active || len > LMESH_MAX_FRAME) {
        return -1;
    }
    memcpy(sn->tx_buf, frame, len);
    sn->tx_len = len;
    sn->tx_active = true;
    sn->tx_done_ms = pc->net->now_ms + port_airtime_ms(ctx, len);
    sn->frames_tx++;
    pc->net->frames_sent++;
    return 0;
}

static uint16_t port_battery_mv(void *ctx)
{
    (void)ctx;
    return 3700;
}

uint32_t sim_rand(sim_net *net)
{
    uint32_t x = net->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    net->rng = x ? x : 0x9E3779B9u;
    return net->rng;
}

void sim_init(sim_net *net, uint32_t seed)
{
    memset(net, 0, sizeof *net);
    net->rng = seed ? seed : 0x9E3779B9u;
    net->airtime_fixed_ms = 15;
    net->airtime_us_per_byte = 1500;
}

int sim_add_node(sim_net *net, lmesh_node_id id, uint8_t network_id, void (*on_event)(void *ctx, const lmesh_event *ev),
                 void (*on_command)(void *ctx, lmesh_node_id from, const uint8_t *data, size_t len,
                                    lmesh_cmd_result *result))
{
    if (net->count >= SIM_MAX_NODES) {
        return -1;
    }
    int i = net->count++;
    sim_node *sn = &net->nodes[i];
    memset(sn, 0, sizeof *sn);
    sn->ctx.net = net;
    sn->ctx.index = i;
    sn->ident.id = id;
    sn->ident.network_id = network_id;

    lmesh_port port = {
        .ctx = &sn->ctx,
        .now_ms = port_now_ms,
        .random = port_random,
        .radio_send = port_radio_send,
        .radio_busy = NULL,
        .airtime_ms = port_airtime_ms,
        .battery_mv = port_battery_mv,
        .health_flags = NULL,
        .storage_read = NULL,
        .storage_write = NULL,
    };
    lmesh_callbacks cb = {.ctx = sn, .on_event = on_event, .on_command = on_command};
    if (lmesh_init(&sn->mesh, &sn->ident, &port, &cb) != LMESH_OK) {
        net->count--;
        return -1;
    }
    sn->alive = true;
    return i;
}

void sim_set_link(sim_net *net, int a, int b, bool up, uint8_t loss_pct)
{
    net->link_up[a][b] = up;
    net->link_up[b][a] = up;
    net->loss_pct[a][b] = loss_pct;
    net->loss_pct[b][a] = loss_pct;
}

void sim_kill(sim_net *net, int i)
{
    net->nodes[i].alive = false;
    net->nodes[i].tx_active = false;
}

void sim_revive(sim_net *net, int i)
{
    sim_node *sn = &net->nodes[i];
    lmesh_port port = sn->mesh.port;
    lmesh_callbacks cb = sn->mesh.cb;
    lmesh_init(&sn->mesh, &sn->ident, &port, &cb);
    sn->alive = true;
}

static void deliver(sim_net *net, int from)
{
    sim_node *tx = &net->nodes[from];
    for (int j = 0; j < net->count; j++) {
        sim_node *rx = &net->nodes[j];
        if (j == from || !rx->alive || !net->link_up[from][j]) {
            continue;
        }
        if (rx->tx_active) {
            net->frames_half_duplex++;
            continue;
        }
        if ((sim_rand(net) % 100u) < net->loss_pct[from][j]) {
            net->frames_lost++;
            continue;
        }
        rx->frames_rx++;
        net->frames_delivered++;
        lmesh_radio_rx(&rx->mesh, tx->tx_buf, tx->tx_len, -80, 8);
    }
    tx->tx_active = false;
    lmesh_radio_tx_done(&tx->mesh, true);
}

void sim_step(sim_net *net, uint32_t ms)
{
    uint32_t target = net->now_ms + ms;

    while ((int32_t)(target - net->now_ms) > 0) {
        uint32_t next = target;
        for (int i = 0; i < net->count; i++) {
            sim_node *sn = &net->nodes[i];
            if (!sn->alive) {
                continue;
            }
            uint32_t wake = net->now_ms + lmesh_next_wake_ms(&sn->mesh);
            if ((int32_t)(wake - next) < 0) {
                next = wake;
            }
            if (sn->tx_active && (int32_t)(sn->tx_done_ms - next) < 0) {
                next = sn->tx_done_ms;
            }
        }
        if ((int32_t)(next - net->now_ms) <= 0) {
            next = net->now_ms + 1;
        }
        net->now_ms = next;

        for (int i = 0; i < net->count; i++) {
            sim_node *sn = &net->nodes[i];
            if (sn->alive && sn->tx_active && (int32_t)(net->now_ms - sn->tx_done_ms) >= 0) {
                deliver(net, i);
            }
        }
        for (int i = 0; i < net->count; i++) {
            if (net->nodes[i].alive) {
                lmesh_tick(&net->nodes[i].mesh);
            }
        }
    }
}
