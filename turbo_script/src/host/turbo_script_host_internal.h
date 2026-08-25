#ifndef TURBO_SCRIPT_HOST_INTERNAL_H
#define TURBO_SCRIPT_HOST_INTERNAL_H

#include "exprtk_module.h"
#include "turbo_buffer.h"
#include "turbo_script_host_api_internal.h"

typedef struct ts_host_value_limits_s {
  uint32_t max_depth;
  size_t max_nodes;
  size_t max_bytes;
} ts_host_value_limits_t;

/* Single-owner result. Getter views are invalidated by reset, the next write,
 * or destroy; every operation must run on the owning context's thread. */
struct turbo_script_result_s {
  turbo_script_ctx_t *ctx;
  mem_pool_t view_arena;
  turbo_script_value_view_t value;
  turbo_script_error_info_t error;
  int has_value;
  int has_error;
};

/* Translate the context's immutable owner fact into the host ABI status. */
turbo_script_status_t ts_host_context_check_thread(const turbo_script_ctx_t *ctx);

/* Checked boundaries underlying the locked void APIs. They never mutate on
 * failure; the public wrappers turn WRONG_THREAD into a contract violation. */
turbo_script_status_t ts_host_result_reset_checked(turbo_script_result_t *result);
turbo_script_status_t ts_host_result_destroy_checked(turbo_script_result_t *result);

/* Validate a borrowed tree without retaining any input pointer. Record keys
 * are non-empty, valid UTF-8, and U+0000-free; string values remain length-based. */
turbo_script_status_t ts_host_validate_value_view(const turbo_script_value_view_t *value,
                                                  const ts_host_value_limits_t *limits);

/* Convert into target_ctx's exprtk ownership domain. On success the caller
 * owns out_value and must destroy it before target_ctx; on failure out_value
 * is EXPRTK_VAL_NULL and owns no resource. */
turbo_script_status_t ts_host_value_from_view(turbo_script_ctx_t *target_ctx,
                                              const turbo_script_value_view_t *value,
                                              const ts_host_value_limits_t *limits,
                                              exprtk_value_t *out_value);

/* Every write invalidates prior getter views first. Publication happens only
 * after complete validation and a successful result-arena deep copy. */
turbo_script_status_t ts_host_result_store_view(turbo_script_result_t *result,
                                                const turbo_script_value_view_t *value,
                                                const ts_host_value_limits_t *limits);

/* Validate every scalar/reserved field and deep-copy all error views into the
 * result arena under max_bytes. */
turbo_script_status_t ts_host_result_set_error(turbo_script_result_t *result,
                                               const turbo_script_error_info_t *error,
                                               size_t max_bytes);

/* Later module/instance entry points use this before touching a result. */
turbo_script_status_t ts_host_result_check_context(const turbo_script_result_t *result,
                                                   const turbo_script_ctx_t *ctx);

#endif /* TURBO_SCRIPT_HOST_INTERNAL_H */
