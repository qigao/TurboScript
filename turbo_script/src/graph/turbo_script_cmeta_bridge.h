#ifndef TURBO_SCRIPT_CMETA_BRIDGE_H
#define TURBO_SCRIPT_CMETA_BRIDGE_H

#include "exprtk_types.h"
#include <cmeta/cmeta.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ts_cmeta_value_shape {
  TS_CMETA_VALUE_SCALAR = 0,
  TS_CMETA_VALUE_VECTOR = 1
} ts_cmeta_value_shape_t;

typedef struct ts_cmeta_value_semantics {
  ts_cmeta_value_shape_t shape;
  const cmeta_type_desc *flow_type;
} ts_cmeta_value_semantics_t;

typedef enum ts_cmeta_lambda_role {
  TS_CMETA_LAMBDA_FILTER = 0,
  TS_CMETA_LAMBDA_MAP = 1,
  TS_CMETA_LAMBDA_REDUCE = 2
} ts_cmeta_lambda_role_t;

/*
 * Analysis-only callable contract for graph admission.
 *
 * The callable deliberately has no executable TurboScript semantics yet:
 * invoke() returns false. Issue #17 owns replacement with a MIR-backed
 * executable adapter. Keeping this seam non-executable prevents graph analysis
 * from accidentally becoming a second interpreter.
 */
typedef struct ts_cmeta_lambda_contract {
  cmeta_callable callable;
  const cmeta_type_desc *input_type;
  const cmeta_type_desc *output_type;
  cmeta_effects effects;
  cmeta_properties properties;
  size_t arity;
  size_t capture_count;
  bool executable;
} ts_cmeta_lambda_contract_t;

/*
 * Describe the element type that enters a CFlow pipeline.
 * TurboScript vectors are homogeneous double vectors, so their flow type is
 * cmeta_type_double even though the script-visible value itself is a vector.
 */
bool ts_cmeta_describe_value_type(exprtk_value_type_t type,
                                  ts_cmeta_value_semantics_t *out);

/*
 * Build an analysis-only CMeta contract for a graphable TurboScript lambda.
 * The first vertical slice is intentionally numeric:
 *   filter: double -> bool
 *   map:    double -> double
 *   reduce: (double, double) -> double
 *
 * Pure arithmetic/comparison expressions over parameters/constants are marked
 * PURE + DETERMINISTIC. Reads of non-parameter variables and dynamic calls are
 * conservatively marked UNKNOWN and therefore cannot participate in
 * purity-dependent CFlow rewrites.
 */
bool ts_cmeta_analyze_lambda(const exprtk_node_t *lambda,
                             ts_cmeta_lambda_role_t role,
                             ts_cmeta_lambda_contract_t *out,
                             const char **error);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SCRIPT_CMETA_BRIDGE_H */
