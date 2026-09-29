#ifndef TURBO_SCRIPT_CFLOW_LOWER_H
#define TURBO_SCRIPT_CFLOW_LOWER_H

#include "exprtk_types.h"
#include "turbo_script_cflow_mir_kernel.h"
#include <cflow/graph.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_cflow_lowered_pipeline {
  cflow_graph graph;
  const exprtk_node_t *source_expr;
  const exprtk_node_t *reduce_seed_expr;
  ts_cflow_mir_kernel_binding_t *kernel_bindings;
  size_t kernel_binding_count;
  size_t kernel_binding_capacity;
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
 * The analysis path retains TurboScript's explicit reduce seed AST for
 * diagnostics. Executable lowering binds MIR callables and materializes a
 * literal numeric reduce seed through CFlow's canonical seeded fold.
 */
bool ts_cflow_lower_pipeline(const exprtk_node_t *expr,
                             ts_cflow_lowered_pipeline_t *out,
                             const char **error);

/*
 * Lower the same graphable slice while binding every supported script lambda
 * to a TurboScript-owned MIR kernel. MAP/FILTER are unary kernels; REDUCE is a
 * binary kernel and uses CFlow's owned typed seed. The first executable reduce
 * slice accepts numeric literal seeds (including unary +/-) and rejects dynamic
 * seed expressions explicitly. This API never falls back to analysis-only
 * callables.
 */
bool ts_cflow_lower_pipeline_executable(turbo_script_ctx_t *runtime_ctx,
                                        const exprtk_node_t *expr,
                                        ts_cflow_lowered_pipeline_t *out,
                                        const char **error);

void ts_cflow_lowered_pipeline_destroy(ts_cflow_lowered_pipeline_t *pipeline);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SCRIPT_CFLOW_LOWER_H */