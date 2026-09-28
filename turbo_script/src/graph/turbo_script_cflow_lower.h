#ifndef TURBO_SCRIPT_CFLOW_LOWER_H
#define TURBO_SCRIPT_CFLOW_LOWER_H

#include "exprtk_types.h"
#include <cflow/graph.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_cflow_lowered_pipeline {
  cflow_graph graph;
  const exprtk_node_t *source_expr;
  const exprtk_node_t *reduce_seed_expr;
  bool has_reduce_seed;
  bool executable;
} ts_cflow_lowered_pipeline_t;

/*
 * Lower the first graphable TurboScript pipeline slice into a CFlow Surface
 * Graph. Pipe syntax is already normalized by the parser into nested function
 * calls, so this pass recognizes:
 *
 *   filter(source, fn)
 *   map(previous, fn)
 *   reduce(previous, seed, fn)
 *
 * The produced graph is analysis-only until #17 supplies MIR-backed callable
 * invoke adapters. TurboScript's explicit reduce seed is retained separately
 * because current CFlow REDUCE is an unseeded fold.
 */
bool ts_cflow_lower_pipeline(const exprtk_node_t *expr,
                             ts_cflow_lowered_pipeline_t *out,
                             const char **error);

void ts_cflow_lowered_pipeline_destroy(ts_cflow_lowered_pipeline_t *pipeline);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SCRIPT_CFLOW_LOWER_H */
