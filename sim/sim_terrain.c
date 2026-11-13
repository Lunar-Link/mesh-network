/* Terrain medium: table parser, model error as a common bias plus a per-pair offset that decorrelates along mobile tracks, and the medium update. */
#include "sim_terrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN 64
#define SNR_MAX_DB 12.0
#define SNR_MIN_DB (-30.0)
#define PI 3.14159265358979323846

static bool fail(char *err, size_t err_max, const char *what, const char *detail)
{
    snprintf(err, err_max, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    return false;
}

static bool word(FILE *f, char *out)
{
    return fscanf(f, "%63s", out) == 1;
}

static bool number(FILE *f, double *out)
{
    return fscanf(f, "%lf", out) == 1;
}

static bool count(FILE *f, uint32_t *out)
{
    unsigned long v;
    if (fscanf(f, "%lu", &v) != 1) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool index_of(FILE *f, int n, int *out)
{
    int v;
    if (fscanf(f, "%d", &v) != 1 || v < 0 || v >= n) {
        return false;
    }
    *out = v;
    return true;
}

static bool series(sim_terrain *t, FILE *f, uint32_t n, uint32_t *off)
{
    if (n == 0 || t->used + n > SIM_TERRAIN_POOL) {
        return false;
    }
    *off = t->used;
    for (uint32_t k = 0; k < n; k++) {
        double v;
        if (!number(f, &v)) {
            return false;
        }
        t->pool[t->used++] = (float)v;
    }
    return true;
}

static bool keyed(FILE *f, const char *key, double *out)
{
    char w[TOKEN];
    return word(f, w) && strcmp(w, key) == 0 && number(f, out);
}

bool terrain_load(sim_terrain *t, const char *path, char *err, size_t err_max)
{
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "r");
    if (!f) {
        return fail(err, err_max, "cannot open terrain table", path);
    }
    char w[TOKEN];
    double v;
    bool ok = word(f, w) && strcmp(w, "lmesh-terrain") == 0 && number(f, &v) && v == 1.0;
    if (!ok) {
        fclose(f);
        return fail(err, err_max, "not a version 1 terrain table", path);
    }
    bool have_nodes = false, have_times = false, ended = false;
    while (ok && !ended && word(f, w)) {
        if (w[0] == '#') {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
        } else if (strcmp(w, "name") == 0) {
            ok = word(f, w);
            snprintf(t->name, sizeof t->name, "%s", w);
        } else if (strcmp(w, "radio") == 0) {
            double f_hz;
            ok = word(f, w) && keyed(f, "f_hz", &f_hz) && keyed(f, "tx_dbm", &t->tx_dbm) &&
                 keyed(f, "sens_dbm", &t->sens_dbm) && keyed(f, "noise_dbm", &t->noise_dbm) &&
                 keyed(f, "per_ref", &t->per_ref) && keyed(f, "per_width_db", &t->per_width_db);
            snprintf(t->preset, sizeof t->preset, "%.15s", w);
            ok = ok && t->per_ref > 0.0 && t->per_ref < 1.0 && t->per_width_db > 0.0;
        } else if (strcmp(w, "lora") == 0) {
            double sf, bw, cr, pre;
            ok = keyed(f, "sf", &sf) && keyed(f, "bw_hz", &bw) && keyed(f, "cr", &cr) && keyed(f, "preamble", &pre) &&
                 sf >= 5 && sf <= 12 && bw > 0 && cr >= 1 && cr <= 4 && pre >= 6;
            if (ok) {
                t->lora.sf = (uint8_t)sf;
                t->lora.bw_hz = (uint32_t)bw;
                t->lora.cr = (uint8_t)cr;
                t->lora.preamble = (uint16_t)pre;
            }
        } else if (strcmp(w, "times") == 0) {
            ok = count(f, &t->times) && number(f, &t->step_s) && t->times > 0 && t->step_s > 0;
            have_times = ok;
        } else if (strcmp(w, "nodes") == 0) {
            uint32_t n = 0;
            ok = count(f, &n) && n >= 2 && n <= SIM_MAX_NODES;
            t->n = (int)n;
            have_nodes = ok;
        } else if (strcmp(w, "node") == 0 && have_nodes) {
            int i = 0;
            double start = 0.0, mobile = 0.0, h = 0.0;
            char name[TOKEN], ant[TOKEN];
            ok = index_of(f, t->n, &i) && word(f, name) && number(f, &start) && number(f, &mobile) && number(f, &h) &&
                 word(f, ant);
            if (ok) {
                snprintf(t->node[i].name, sizeof t->node[i].name, "%.15s", name);
                t->node[i].start_s = start;
                t->node[i].mobile = mobile != 0.0;
            }
        } else if (strcmp(w, "xy") == 0 && have_nodes) {
            int i = 0;
            uint32_t n = 0;
            ok = index_of(f, t->n, &i) && count(f, &n) && series(t, f, 2u * n, &t->node[i].xy_off);
            t->node[i].xy_n = ok ? n : 0u;
        } else if (strcmp(w, "dist") == 0 && have_nodes) {
            int i = 0;
            uint32_t n = 0;
            ok = index_of(f, t->n, &i) && count(f, &n) && series(t, f, n, &t->node[i].dist_off);
            t->node[i].dist_n = ok ? n : 0u;
        } else if (strcmp(w, "link") == 0 && have_nodes && have_times) {
            int a = 0, b = 0;
            uint32_t n = 0;
            ok = index_of(f, t->n, &a) && index_of(f, t->n, &b) && a != b && count(f, &n) && (n == 1 || n == t->times) &&
                 series(t, f, n, &t->link_off[a][b]);
            t->link_n[a][b] = ok ? n : 0u;
        } else if (strcmp(w, "end") == 0) {
            ended = true;
        } else {
            ok = false;
        }
    }
    fclose(f);
    if (!ok || !ended) {
        return fail(err, err_max, ok ? "terrain table has no end line" : "terrain table parse error near", ok ? path : w);
    }
    if (!have_nodes || !have_times || t->lora.sf == 0 || t->tx_dbm == 0.0) {
        return fail(err, err_max, "terrain table is missing nodes, times, radio or lora", path);
    }
    for (int a = 0; a < t->n; a++) {
        for (int b = 0; b < t->n; b++) {
            if (a != b && t->link_n[a][b] == 0) {
                char pair[32];
                snprintf(pair, sizeof pair, "%d -> %d", a, b);
                return fail(err, err_max, "terrain table has no link", pair);
            }
        }
        if (t->node[a].xy_n == 0) {
            return fail(err, err_max, "terrain table has no position for node", t->node[a].name);
        }
    }
    t->loaded = true;
    return true;
}

static double at(const sim_terrain *t, uint32_t off, uint32_t n, uint32_t stride, double t_s)
{
    if (n == 1) {
        return t->pool[off];
    }
    double u = t_s / t->step_s;
    if (u <= 0.0) {
        return t->pool[off];
    }
    if (u >= (double)(n - 1)) {
        return t->pool[off + (n - 1) * stride];
    }
    uint32_t k = (uint32_t)u;
    double fr = u - (double)k;
    return (1.0 - fr) * t->pool[off + k * stride] + fr * t->pool[off + (k + 1) * stride];
}

double terrain_end_s(const sim_terrain *t)
{
    return (double)(t->times - 1) * t->step_s;
}

double terrain_loss_db(const sim_terrain *t, int a, int b, double t_s)
{
    return at(t, t->link_off[a][b], t->link_n[a][b], 1, t_s);
}

void terrain_position(const sim_terrain *t, int i, double t_s, double *x, double *y)
{
    const sim_terrain_node *nd = &t->node[i];
    *x = at(t, nd->xy_off, nd->xy_n, 2, t_s);
    *y = at(t, nd->xy_off + 1, nd->xy_n, 2, t_s);
}

static double travelled(const sim_terrain *t, int i, double t_s)
{
    const sim_terrain_node *nd = &t->node[i];
    return nd->dist_n ? at(t, nd->dist_off, nd->dist_n, 1, t_s) : 0.0;
}

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x6C8E9CF5u;
    return *s;
}

static double gauss(uint32_t *s)
{
    double u1 = ((double)(xorshift(s) >> 8) + 1.0) / 16777217.0;
    double u2 = (double)(xorshift(s) >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2);
}

void terrain_draw(sim_terrain *t, uint32_t seed, double sigma_db, double bias_db, double decor_m)
{
    uint32_t s = seed * 0x9E3779B1u ^ 0x85EBCA6Bu;
    for (int k = 0; k < 8; k++) {
        xorshift(&s);
    }
    t->sigma_db = sigma_db;
    t->bias_db = bias_db;
    t->decor_m = decor_m > 1.0 ? decor_m : 1.0;
    t->bias = bias_db > 0.0 ? bias_db * gauss(&s) : 0.0;
    for (int a = 0; a < SIM_MAX_NODES; a++) {
        for (int b = a + 1; b < SIM_MAX_NODES; b++) {
            for (int k = 0; k < SIM_TERRAIN_KNOTS; k++) {
                t->knot[a][b][k] = sigma_db > 0.0 ? (float)gauss(&s) : 0.0f;
            }
        }
    }
}

double terrain_error_db(const sim_terrain *t, int a, int b, double t_s)
{
    if (t->sigma_db <= 0.0) {
        return t->bias;
    }
    int lo = a < b ? a : b, hi = a < b ? b : a;
    const float *g = t->knot[lo][hi];
    if (!t->node[lo].mobile && !t->node[hi].mobile) {
        return t->bias + t->sigma_db * g[0];
    }
    double u = (travelled(t, lo, t_s) + travelled(t, hi, t_s)) / t->decor_m;
    if (u > (double)(SIM_TERRAIN_KNOTS - 2)) {
        u = fmod(u, (double)(SIM_TERRAIN_KNOTS - 2));
    }
    uint32_t k = (uint32_t)u;
    double w = (u - (double)k) * PI / 2.0;
    return t->bias + t->sigma_db * (cos(w) * g[k] + sin(w) * g[k + 1]);
}

double terrain_margin_db(const sim_terrain *t, int a, int b, double t_s)
{
    return t->tx_dbm - terrain_loss_db(t, a, b, t_s) - terrain_error_db(t, a, b, t_s) - t->sens_dbm;
}

double terrain_per(const sim_terrain *t, double margin_db)
{
    double z = margin_db / t->per_width_db;
    if (z > 40.0) {
        return 0.0;
    }
    return 1.0 / (1.0 + (1.0 - t->per_ref) / t->per_ref * exp(z));
}

static uint8_t loss_pct(double per)
{
    double p = floor(per * 100.0 + 0.5);
    return (uint8_t)(p < 0.0 ? 0.0 : p > 100.0 ? 100.0 : p);
}

void terrain_apply(const sim_terrain *t, sim_net *net, double t_s)
{
    int n = t->n < net->count ? t->n : net->count;
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            double m_ab = terrain_margin_db(t, a, b, t_s), m_ba = terrain_margin_db(t, b, a, t_s);
            uint8_t p_ab = loss_pct(terrain_per(t, m_ab)), p_ba = loss_pct(terrain_per(t, m_ba));
            bool up = p_ab < 100u || p_ba < 100u;
            if (up != net->link_up[a][b]) {
                sim_set_link(net, a, b, up, 0);
            }
            sim_set_loss_dir(net, a, b, p_ab);
            sim_set_loss_dir(net, b, a, p_ba);
            for (int d = 0; d < 2; d++) {
                int from = d ? b : a, to = d ? a : b;
                double rx = t->sens_dbm + (d ? m_ba : m_ab);
                double snr = rx - t->noise_dbm;
                snr = snr > SNR_MAX_DB ? SNR_MAX_DB : snr < SNR_MIN_DB ? SNR_MIN_DB : snr;
                rx = rx < -200.0 ? -200.0 : rx;
                sim_set_signal(net, from, to, (int16_t)floor(rx + 0.5), (int8_t)floor(snr + 0.5));
            }
        }
    }
}

bool terrain_connected(const sim_terrain *t, const sim_net *net, int a, int b, double t_s, double min_margin_db)
{
    int n = t->n < net->count ? t->n : net->count;
    bool seen[SIM_MAX_NODES] = {false};
    int queue[SIM_MAX_NODES], head = 0, tail = 0;
    if (!net->nodes[a].alive || !net->nodes[b].alive) {
        return false;
    }
    seen[a] = true;
    queue[tail++] = a;
    while (head < tail) {
        int u = queue[head++];
        if (u == b) {
            return true;
        }
        for (int v = 0; v < n; v++) {
            if (!seen[v] && v != u && net->nodes[v].alive && terrain_margin_db(t, u, v, t_s) >= min_margin_db &&
                terrain_margin_db(t, v, u, t_s) >= min_margin_db) {
                seen[v] = true;
                queue[tail++] = v;
            }
        }
    }
    return false;
}
