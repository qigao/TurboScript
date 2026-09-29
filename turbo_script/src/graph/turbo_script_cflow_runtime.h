#ifndef TURBO_SCRIPT_CFLOW_RUNTIME_H
#define TURBO_SCRIPT_CFLOW_RUNTIME_H

#include "exprtk_types.h"
#include "turbo_script.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ts_cflow_runtime_status_e {
  TS_CFLOW_RUNTIME_NOT_APPLICABLE = 0,
  TS_CFLOW_RUNTIME_HANDLED = 1,
  TS_CFLOW_RUNTIME_ERROR = -1
} ts_cflow_runtime_status_t;

/*
 * Execute the runtime-owned CFlow stream facade slice.
 *
 * Eligible expressions are numeric in-memory pipelines sourced from vector
 * literals, bound vectors, or homogeneous numeric lists and terminating in
 * reduce/count/collect/toList/toVector. collect preserves the legacy vector
 * vs list result shape: map-free vector pipelines collect as vector, while a
 * pipeline containing map collects as list. Unsupported stream shapes remain
 * owned by the legacy facade until their migration slice lands. Once an
 * expression is admitted as graphable, execution errors are reported and
 * never fall back.
 *
 * The Phase 2 text seam additionally admits the exact terminal shapes
 * stream.text(expr).lines().count()/collect()/toList(). The text expression is
 * evaluated once, line slices borrow that string only for synchronous Plan
 * evaluation, and list terminals copy strings into TurboScript-owned storage
 * before the CFlow result/source text are released. Legacy newline/CRLF/
 * trailing-empty-line semantics are preserved.
 */
ts_cflow_runtime_status_t ts_cflow_runtime_try_scalar_terminal(
    turbo_script_ctx_t *ctx, const exprtk_node_t *expr, exprtk_value_t *out,
    char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SCRIPT_CFLOW_RUNTIME_H */