/**
 * @file exprtk_graph.c
 * @brief Graph algorithms for financial network analysis
 *
 * Provides graph algorithms including:
 * - Bellman-Ford single-source shortest path
 * - Negative cycle detection (arbitrage detection)
 * - Path extraction
 */

#include "fin.h"
#include "exprtk.h"
#include "exprtk_types.h"
#include <float.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Bellman-Ford Algorithm
 * ========================================================================= */

/**
 * @brief Bellman-Ford single-source shortest path algorithm.
 *
 * Computes shortest paths from source to all vertices, handles negative weights.
 *
 * @param edges Flat array [from, to, weight, from, to, weight, ...]
 * @param n_vertices Number of vertices (0 to n_vertices-1)
 * @param n_edges Number of edges
 * @param source Source vertex
 * @param dist Output: distance array [n_vertices]
 * @param prev Output: predecessor array [n_vertices] (-1 if no predecessor)
 * @param arena Memory arena
 * @return 0 on success, -1 if negative cycle detected
 */
static int bellman_ford_impl(const double *edges, size_t n_vertices, size_t n_edges,
                              size_t source, double *dist, double *prev,
                              mem_pool_t *arena) {
    (void)arena;

    if (!edges || !dist || !prev || n_vertices == 0 || source >= n_vertices) {
        return -1;
    }

    /* Initialize distances */
    for (size_t i = 0; i < n_vertices; i++) {
        dist[i] = DBL_MAX;
        prev[i] = -1.0;
    }
    dist[source] = 0.0;

    /* Relax edges V-1 times */
    for (size_t iter = 0; iter < n_vertices - 1; iter++) {
        int updated = 0;
        for (size_t e = 0; e < n_edges; e++) {
            size_t u = (size_t)edges[e * 3 + 0];
            size_t v = (size_t)edges[e * 3 + 1];
            double w = edges[e * 3 + 2];

            if (u >= n_vertices || v >= n_vertices) continue;
            if (dist[u] == DBL_MAX) continue;

            if (dist[u] + w < dist[v]) {
                dist[v] = dist[u] + w;
                prev[v] = (double)u;
                updated = 1;
            }
        }
        /* Early termination if no updates */
        if (!updated) break;
    }

    /* Check for negative cycles */
    for (size_t e = 0; e < n_edges; e++) {
        size_t u = (size_t)edges[e * 3 + 0];
        size_t v = (size_t)edges[e * 3 + 1];
        double w = edges[e * 3 + 2];

        if (u >= n_vertices || v >= n_vertices) continue;
        if (dist[u] == DBL_MAX) continue;

        /* Use epsilon tolerance to avoid false positives from floating point errors */
        if (dist[u] + w < dist[v] - 1e-9) {
            return -1;  /* Negative cycle detected */
        }
    }

    return 0;
}

/**
 * @brief exprtk wrapper for Bellman-Ford algorithm.
 */
exprtk_value_t exprtk_graph_bellman_ford(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;

    if (argc != 6) {
        return exprtk_val_num(NAN);
    }

    /* Parse arguments */
    if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER ||
        args[2].type != EXPRTK_VAL_NUMBER || args[3].type != EXPRTK_VAL_NUMBER ||
        args[4].type != EXPRTK_VAL_VECTOR || args[5].type != EXPRTK_VAL_VECTOR) {
        return exprtk_val_num(NAN);
    }

    const double *edges = args[0].data.vector.data;
    size_t edges_len = args[0].data.vector.size;
    size_t n_vertices = (size_t)args[1].data.number;
    size_t n_edges = (size_t)args[2].data.number;
    size_t source = (size_t)args[3].data.number;
    double *dist = args[4].data.vector.data;
    size_t dist_len = args[4].data.vector.size;
    double *prev = args[5].data.vector.data;
    size_t prev_len = args[5].data.vector.size;

    /* Validate */
    if (edges_len < n_edges * 3 || dist_len < n_vertices || prev_len < n_vertices) {
        return exprtk_val_num(NAN);
    }

    /* Run algorithm */
    int result = bellman_ford_impl(edges, n_vertices, n_edges, source, dist, prev, arena);

    return exprtk_val_num((double)result);
}

/* =========================================================================
 * Negative Cycle Detection
 * ========================================================================= */

exprtk_value_t exprtk_graph_has_negative_cycle(size_t argc, exprtk_value_t *args,
                                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;

    if (argc != 3) {
        return exprtk_val_num(NAN);
    }

    if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER ||
        args[2].type != EXPRTK_VAL_NUMBER) {
        return exprtk_val_num(NAN);
    }

    const double *edges = args[0].data.vector.data;
    size_t n_vertices = (size_t)args[1].data.number;
    size_t n_edges = (size_t)args[2].data.number;

    /* Allocate temporary arrays */
    double *dist = MEM_ALLOC_ARRAY(arena, double, n_vertices);
    double *prev = MEM_ALLOC_ARRAY(arena, double, n_vertices);
    if (!dist || !prev) {
        return exprtk_val_num(NAN);
    }

    /* Run Bellman-Ford from vertex 0 */
    int result = bellman_ford_impl(edges, n_vertices, n_edges, 0, dist, prev, arena);

    return exprtk_val_num(result == -1 ? 1.0 : 0.0);
}

/* =========================================================================
 * Path Extraction
 * ========================================================================= */

exprtk_value_t exprtk_graph_extract_path(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc != 4) {
        return exprtk_val_num(NAN);
    }

    if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER ||
        args[2].type != EXPRTK_VAL_NUMBER || args[3].type != EXPRTK_VAL_VECTOR) {
        return exprtk_val_num(NAN);
    }

    const double *prev = args[0].data.vector.data;
    size_t prev_len = args[0].data.vector.size;
    size_t source = (size_t)args[1].data.number;
    size_t target = (size_t)args[2].data.number;
    double *path = args[3].data.vector.data;
    size_t path_capacity = args[3].data.vector.size;

    if (source >= prev_len || target >= prev_len) {
        return exprtk_val_num(-1.0);
    }

    /* Check if path exists */
    if (prev[target] < 0 && target != source) {
        return exprtk_val_num(-1.0);  /* No path */
    }

    /* Build path backwards */
    size_t path_len = 0;
    size_t current = target;

    while (current != source && path_len < path_capacity) {
        path[path_len++] = (double)current;
        if (prev[current] < 0) {
            return exprtk_val_num(-1.0);  /* Broken path */
        }
        current = (size_t)prev[current];

        /* Cycle detection */
        if (path_len > prev_len) {
            return exprtk_val_num(-1.0);
        }
    }

    if (path_len < path_capacity) {
        path[path_len++] = (double)source;
    }

    /* Reverse path to get source -> target order */
    for (size_t i = 0; i < path_len / 2; i++) {
        double temp = path[i];
        path[i] = path[path_len - 1 - i];
        path[path_len - 1 - i] = temp;
    }

    return exprtk_val_num((double)path_len);
}

/* =========================================================================
 * Currency Arbitrage Detection
 * ========================================================================= */

exprtk_value_t exprtk_graph_detect_arbitrage(size_t argc, exprtk_value_t *args,
                                              exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;

    if (argc != 2) {
        return exprtk_val_num(NAN);
    }

    if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER) {
        return exprtk_val_num(NAN);
    }

    const double *rates = args[0].data.vector.data;
    size_t rates_len = args[0].data.vector.size;
    size_t n = (size_t)args[1].data.number;

    if (rates_len < n * n) {
        return exprtk_val_num(NAN);
    }

    /* Build edge list with -log(rate) weights */
    size_t n_edges = n * (n - 1);  /* Complete graph minus self-loops */
    double *edges = MEM_ALLOC_ARRAY(arena, double, n_edges * 3);
    if (!edges) {
        return exprtk_val_num(NAN);
    }

    size_t edge_idx = 0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            if (i == j) continue;
            double rate = rates[i * n + j];
            if (rate <= 0.0) continue;  /* Invalid rate */

            edges[edge_idx * 3 + 0] = (double)i;
            edges[edge_idx * 3 + 1] = (double)j;
            edges[edge_idx * 3 + 2] = -log(rate);
            edge_idx++;
        }
    }

    /* Allocate temporary arrays */
    double *dist = MEM_ALLOC_ARRAY(arena, double, n);
    double *prev = MEM_ALLOC_ARRAY(arena, double, n);
    if (!dist || !prev) {
        return exprtk_val_num(NAN);
    }

    /* Check for negative cycle */
    int result = bellman_ford_impl(edges, n, edge_idx, 0, dist, prev, arena);

    return exprtk_val_num(result == -1 ? 1.0 : 0.0);
}
