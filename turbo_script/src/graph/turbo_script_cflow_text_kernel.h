#ifndef TURBO_SCRIPT_CFLOW_TEXT_KERNEL_H
#define TURBO_SCRIPT_CFLOW_TEXT_KERNEL_H

#include "exprtk_types.h"
#include "turbo_script_cflow_text.h"

#include <cflow/graph.h>
#include <cmeta/cmeta.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_cflow_text_kernel_s ts_cflow_text_kernel_t;

typedef struct ts_cflow_text_kernel_binding {
  cmeta_callable callable;
  ts_cflow_text_kernel_t *owner;
} ts_cflow_text_kernel_binding_t;

typedef struct ts_cflow_text_function_reflection {
  cmeta_type_desc input_ptr_type;
  cmeta_param_desc param;
  cmeta_function_desc function;
  cmeta_abi_carrier param_abi[1];
  cmeta_function_abi_desc abi;
} ts_cflow_text_function_reflection_t;

/*
 * Bind the first typed text callable slice:
 *
 *   line => line.length()
 *
 * Logical CFlow type:
 *   TurboScript.LineSlice.v1 -> double
 *
 * The executable adapter is erased at the CMeta callable boundary
 * (meta.sig == CMETA_SIG_INVALID), but CFlow admission supplies exact
 * input/output descriptors through its typed-adapter projection.
 *
 * MIR ABI:
 *   double kernel(const char *data, int64_t len)
 *
 * data and len are marshaled from the LineSlice descriptor. No pointer is
 * coerced through double and no ExprTk runtime value layout crosses the ABI.
 */
bool ts_cflow_text_length_map_bind(
    const exprtk_node_t *lambda,
    ts_cflow_text_kernel_binding_t *out,
    const char **error);

/*
 * Build the exact reflected FunctionDesc/FunctionAbi used by typed MAP
 * admission. The scalar double result is a canonical VALUE; the LineSlice
 * parameter remains an IN|BORROWED object-pointer carrier.
 */
bool ts_cflow_text_length_map_reflect(
    const ts_cflow_text_kernel_binding_t *binding,
    ts_cflow_text_function_reflection_t *out);

/* Add the bound callable as an explicit typed MAP node. */
bool ts_cflow_text_length_map_graph_add(
    cflow_graph *graph,
    const ts_cflow_text_kernel_binding_t *binding,
    const char **error);

/*
 * First typed text FILTER slice:
 *
 *   line => line.length() > 0
 *
 * Logical CFlow type:
 *   TurboScript.LineSlice.v1 -> bool
 *
 * MIR ABI uses an integer predicate carrier:
 *   int64_t kernel(const char *data, int64_t len)
 *
 * The erased CMeta adapter converts nonzero to canonical _Bool. CFlow's typed
 * FILTER projection preserves LineSlice as the Graph output element type.
 */
bool ts_cflow_text_nonempty_filter_bind(
    const exprtk_node_t *lambda,
    ts_cflow_text_kernel_binding_t *out,
    const char **error);

bool ts_cflow_text_nonempty_filter_reflect(
    const ts_cflow_text_kernel_binding_t *binding,
    ts_cflow_text_function_reflection_t *out);

bool ts_cflow_text_nonempty_filter_graph_add(
    cflow_graph *graph,
    const ts_cflow_text_kernel_binding_t *binding,
    const char **error);

void ts_cflow_text_kernel_binding_destroy(
    ts_cflow_text_kernel_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SCRIPT_CFLOW_TEXT_KERNEL_H */
