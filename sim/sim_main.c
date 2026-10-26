/* Scenario runner: lmesh_sim [-v] <scenario> [seed] [csv] | sweep <scenario> <count> [csv] [--known file] | regress <file>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_scenarios.h"

#define EXIT_FAILED 1
#define EXIT_UNKNOWN 2
#define EXIT_UNIMPLEMENTED 3
#define MAX_ARGS 8
#define MAX_KNOWN 1024
#define MAX_FAILED 4096

typedef struct known_seed {
    char scenario[16];
    uint32_t seed;
} known_seed;

static sim_run run;
static known_seed known[MAX_KNOWN];
static size_t known_count;

static int usage(void)
{
    fprintf(stderr, "usage: lmesh_sim [-v] <scenario> [seed] [csv_path]\n"
                    "       lmesh_sim [-v] sweep <scenario> <seed_count> [csv_path] [--known <seed_file>]\n"
                    "       lmesh_sim [-v] regress <seed_file>\n"
                    "scenarios: ");
    sim_list_scenarios(stderr);
    return EXIT_UNKNOWN;
}

static const sim_scenario *lookup(const char *name, int *code)
{
    const sim_scenario *sc = sim_find_scenario(name);
    if (!sc) {
        fprintf(stderr, "unknown scenario: %s\n", name);
        *code = usage();
        return NULL;
    }
    if (!sc->fn) {
        printf("%s: not implemented\n", name);
        *code = EXIT_UNIMPLEMENTED;
        return NULL;
    }
    return sc;
}

static bool load_known(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    char name[16];
    unsigned long seed;
    while (known_count < MAX_KNOWN && fscanf(f, "%15s %lu", name, &seed) == 2) {
        snprintf(known[known_count].scenario, sizeof known[known_count].scenario, "%s", name);
        known[known_count].seed = (uint32_t)seed;
        known_count++;
    }
    fclose(f);
    return true;
}

static bool is_known(const char *name, uint32_t seed)
{
    for (size_t k = 0; k < known_count; k++) {
        if (known[k].seed == seed && strcmp(known[k].scenario, name) == 0) {
            return true;
        }
    }
    return false;
}

static int single(const char *name, uint32_t seed, const char *csv, bool verbose)
{
    int code = 0;
    const sim_scenario *sc = lookup(name, &code);
    if (!sc) {
        return code;
    }
    sim_run_scenario(&run, sc, seed, verbose);
    metrics_print(&run.m);
    metrics_summary(&run.m);
    if (csv) {
        metrics_csv_append(&run.m, csv);
        msgobs_csv_append(&run.obs.msg, &run.m, csv);
    }
    return run.m.passed ? 0 : EXIT_FAILED;
}

static void print_seeds(const char *label, const uint32_t *seeds, uint32_t n)
{
    printf("%s:", label);
    for (uint32_t k = 0; k < n; k++) {
        printf(" %lu", (unsigned long)seeds[k]);
    }
    printf("%s\n", n ? "" : " none");
}

static int sweep(const char *name, uint32_t count, const char *csv, bool verbose)
{
    int code = 0;
    const sim_scenario *sc = lookup(name, &code);
    if (!sc) {
        return code;
    }
    static bool have[M_COUNT];
    static double worst[M_COUNT];
    static double sum[M_COUNT];
    static uint32_t n_set[M_COUNT];
    static uint32_t worst_seed[M_COUNT];
    static uint32_t failed[MAX_FAILED];
    static uint32_t skipped[MAX_FAILED];
    uint32_t n_failed = 0;
    uint32_t n_skipped = 0;
    uint32_t runs = 0;

    for (uint32_t seed = 1; runs < count; seed++) {
        if (is_known(name, seed)) {
            if (n_skipped < MAX_FAILED) {
                skipped[n_skipped++] = seed;
            }
            continue;
        }
        runs++;
        sim_run_scenario(&run, sc, seed, verbose);
        if (verbose || !run.m.passed) {
            metrics_summary(&run.m);
        }
        if (csv) {
            metrics_csv_append(&run.m, csv);
            msgobs_csv_append(&run.obs.msg, &run.m, csv);
        }
        if (!run.m.passed && n_failed < MAX_FAILED) {
            failed[n_failed++] = seed;
        }
        for (int i = 0; i < M_COUNT; i++) {
            if (!run.m.set[i]) {
                continue;
            }
            double v = run.m.v[i];
            bool lower = metric_lower_is_worse((sim_metric_id)i);
            if (!have[i] || (lower ? v < worst[i] : v > worst[i])) {
                have[i] = true;
                worst[i] = v;
                worst_seed[i] = seed;
            }
            sum[i] += v;
            n_set[i]++;
        }
    }

    printf("sweep %s seeds=%lu passed=%lu failed=%lu skipped_known=%lu\n", name, (unsigned long)runs,
           (unsigned long)(runs - n_failed), (unsigned long)n_failed, (unsigned long)n_skipped);
    char w[32], mean[32];
    for (int i = 0; i < M_COUNT; i++) {
        if (have[i]) {
            metric_format(worst[i], w, sizeof w);
            metric_format(sum[i] / n_set[i], mean, sizeof mean);
            printf("worst %s=%s seed=%lu mean=%s runs=%lu\n", metric_name((sim_metric_id)i), w,
                   (unsigned long)worst_seed[i], mean, (unsigned long)n_set[i]);
        }
    }
    if (n_skipped) {
        print_seeds("skipped known failing seeds", skipped, n_skipped);
    }
    print_seeds("failing seeds", failed, n_failed);
    return n_failed ? EXIT_FAILED : 0;
}

static int regress(bool verbose)
{
    size_t n_failed = 0;
    for (size_t k = 0; k < known_count; k++) {
        int code = 0;
        const sim_scenario *sc = lookup(known[k].scenario, &code);
        if (!sc) {
            n_failed++;
            continue;
        }
        sim_run_scenario(&run, sc, known[k].seed, verbose);
        metrics_summary(&run.m);
        n_failed += run.m.passed ? 0u : 1u;
    }
    printf("regress runs=%lu failed=%lu\n", (unsigned long)known_count, (unsigned long)n_failed);
    return n_failed ? EXIT_FAILED : 0;
}

int main(int argc, char **argv)
{
    const char *env = getenv("LMESH_SIM_VERBOSE");
    bool verbose = env && env[0] != '\0' && strcmp(env, "0") != 0;
    const char *args[MAX_ARGS] = {NULL};
    const char *known_path = NULL;
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--known") == 0 && i + 1 < argc) {
            known_path = argv[++i];
        } else if (n < MAX_ARGS) {
            args[n++] = argv[i];
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 4096);
    if (known_path && !load_known(known_path)) {
        return EXIT_UNKNOWN;
    }

    const char *name = n > 0 ? args[0] : "smoke";
    if (strcmp(name, "sweep") == 0) {
        if (n < 3) {
            return usage();
        }
        uint32_t count = (uint32_t)strtoul(args[2], NULL, 10);
        return sweep(args[1], count, n > 3 ? args[3] : NULL, verbose);
    }
    if (strcmp(name, "regress") == 0) {
        if (n < 2 || !load_known(args[1])) {
            return usage();
        }
        return regress(verbose);
    }
    uint32_t seed = n > 1 ? (uint32_t)strtoul(args[1], NULL, 10) : 1u;
    return single(name, seed, n > 2 ? args[2] : NULL, verbose);
}
