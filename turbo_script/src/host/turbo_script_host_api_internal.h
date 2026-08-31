#ifndef TURBO_SCRIPT_HOST_API_INTERNAL_H
#define TURBO_SCRIPT_HOST_API_INTERNAL_H

#include "turbo_script.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_SCRIPT_HOST_ABI_VERSION 1u

typedef int32_t turbo_script_status_t;
typedef uint32_t turbo_script_value_kind_t;
typedef uint32_t turbo_script_error_phase_t;
typedef uint32_t turbo_script_execution_mode_t;
typedef uint64_t turbo_script_export_handle_t;

typedef struct turbo_script_module_s turbo_script_module_t;
typedef struct turbo_script_instance_s turbo_script_instance_t;
typedef struct turbo_script_result_s turbo_script_result_t;
typedef struct turbo_script_host_result_builder_s turbo_script_host_result_builder_t;

typedef struct turbo_script_string_view_s {
  const char *data;
  size_t size;
} turbo_script_string_view_t;

typedef struct turbo_script_value_view_s turbo_script_value_view_t;
typedef struct turbo_script_record_entry_view_s turbo_script_record_entry_view_t;

typedef struct turbo_script_array_view_s {
  const turbo_script_value_view_t *items;
  size_t count;
} turbo_script_array_view_t;

typedef struct turbo_script_record_view_s {
  const turbo_script_record_entry_view_t *entries;
  size_t count;
} turbo_script_record_view_t;

struct turbo_script_value_view_s {
  turbo_script_value_kind_t kind;
  uint32_t reserved;
  union {
    uint8_t boolean;
    int64_t integer;
    double number;
    turbo_script_string_view_t string;
    turbo_script_array_view_t array;
    turbo_script_record_view_t record;
  } as;
};

struct turbo_script_record_entry_view_s {
  turbo_script_string_view_t key;
  turbo_script_value_view_t value;
};

typedef struct turbo_script_module_options_s {
  uint32_t struct_size;
  uint32_t reserved0;
  turbo_script_string_view_t module_name;
  size_t max_source_bytes;
  size_t max_ast_nodes;
  size_t max_imports;
  size_t max_exports;
  size_t max_string_bytes;
  uint64_t reserved[4];
} turbo_script_module_options_t;

typedef struct turbo_script_instance_options_s {
  uint32_t struct_size;
  turbo_script_execution_mode_t mode;
  size_t max_retained_bytes;
  size_t max_stack_bytes;
  uint32_t max_recursion;
  uint32_t max_globals;
  uint32_t max_value_depth;
  uint32_t reserved0;
  size_t max_value_nodes;
  size_t max_result_bytes;
  uint64_t reserved[4];
} turbo_script_instance_options_t;

typedef int (*turbo_script_interrupt_fn)(void *user_data);

typedef struct turbo_script_call_options_s {
  uint32_t struct_size;
  uint32_t max_recursion;
  uint32_t max_steps;
  uint32_t max_loop_iterations;
  uint32_t max_host_callbacks;
  uint32_t reserved0;
  size_t max_result_bytes;
  turbo_script_interrupt_fn interrupt;
  void *interrupt_user_data;
  uint64_t reserved[4];
} turbo_script_call_options_t;

typedef struct turbo_script_export_info_s {
  uint32_t struct_size;
  uint32_t min_arity;
  uint32_t max_arity;
  uint32_t reserved0;
  turbo_script_string_view_t name;
  uint64_t reserved[4];
} turbo_script_export_info_t;

typedef struct turbo_script_error_info_s {
  uint32_t struct_size;
  turbo_script_status_t status;
  int32_t error_code;
  turbo_script_error_phase_t phase;
  uint32_t line;
  uint32_t column;
  uint32_t length;
  int32_t cause_code;
  turbo_script_string_view_t module_name;
  turbo_script_string_view_t function_name;
  turbo_script_string_view_t message;
  uint64_t reserved[4];
} turbo_script_error_info_t;

typedef struct turbo_script_host_function_descriptor_s {
  uint32_t struct_size;
  uint32_t min_arity;
  uint32_t max_arity;
  uint32_t reserved0;
  turbo_script_string_view_t name;
  uint64_t reserved[4];
} turbo_script_host_function_descriptor_t;

typedef turbo_script_status_t (*turbo_script_host_function_t)(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder);

#define TURBO_SCRIPT_STATUS_OK ((turbo_script_status_t)0)
#define TURBO_SCRIPT_STATUS_INVALID_ARGUMENT ((turbo_script_status_t)1)
#define TURBO_SCRIPT_STATUS_OUT_OF_MEMORY ((turbo_script_status_t)2)
#define TURBO_SCRIPT_STATUS_WRONG_THREAD ((turbo_script_status_t)3)
#define TURBO_SCRIPT_STATUS_INVALID_STATE ((turbo_script_status_t)4)
#define TURBO_SCRIPT_STATUS_CONTEXT_MISMATCH ((turbo_script_status_t)5)
#define TURBO_SCRIPT_STATUS_PARSE_ERROR ((turbo_script_status_t)6)
#define TURBO_SCRIPT_STATUS_VALIDATION_ERROR ((turbo_script_status_t)7)
#define TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED ((turbo_script_status_t)8)
#define TURBO_SCRIPT_STATUS_INVALID_UTF8 ((turbo_script_status_t)9)
#define TURBO_SCRIPT_STATUS_DUPLICATE_RECORD_KEY ((turbo_script_status_t)10)
#define TURBO_SCRIPT_STATUS_NOT_FOUND ((turbo_script_status_t)11)
#define TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE ((turbo_script_status_t)12)
#define TURBO_SCRIPT_STATUS_UNSUPPORTED_BACKEND_SEMANTIC ((turbo_script_status_t)13)
#define TURBO_SCRIPT_STATUS_RUNTIME_ERROR ((turbo_script_status_t)14)
#define TURBO_SCRIPT_STATUS_HOST_ERROR ((turbo_script_status_t)15)
#define TURBO_SCRIPT_STATUS_REENTRANT_CALL ((turbo_script_status_t)16)
#define TURBO_SCRIPT_STATUS_INTERRUPTED ((turbo_script_status_t)17)

#define TURBO_SCRIPT_VALUE_NULL ((turbo_script_value_kind_t)0)
#define TURBO_SCRIPT_VALUE_BOOL ((turbo_script_value_kind_t)1)
#define TURBO_SCRIPT_VALUE_INT64 ((turbo_script_value_kind_t)2)
#define TURBO_SCRIPT_VALUE_NUMBER ((turbo_script_value_kind_t)3)
#define TURBO_SCRIPT_VALUE_STRING ((turbo_script_value_kind_t)4)
#define TURBO_SCRIPT_VALUE_ARRAY ((turbo_script_value_kind_t)5)
#define TURBO_SCRIPT_VALUE_RECORD ((turbo_script_value_kind_t)6)

#define TURBO_SCRIPT_ERROR_PHASE_NONE ((turbo_script_error_phase_t)0)
#define TURBO_SCRIPT_ERROR_PHASE_COMPILE ((turbo_script_error_phase_t)1)
#define TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE ((turbo_script_error_phase_t)2)
#define TURBO_SCRIPT_ERROR_PHASE_RESOLVE ((turbo_script_error_phase_t)3)
#define TURBO_SCRIPT_ERROR_PHASE_CALL ((turbo_script_error_phase_t)4)
#define TURBO_SCRIPT_ERROR_PHASE_HOST_CALLBACK ((turbo_script_error_phase_t)5)
#define TURBO_SCRIPT_ERROR_PHASE_INTERRUPT ((turbo_script_error_phase_t)6)

#define TURBO_SCRIPT_EXEC_INTERPRETER ((turbo_script_execution_mode_t)1)
#define TURBO_SCRIPT_EXEC_JIT ((turbo_script_execution_mode_t)2)

void turbo_script_module_options_init(turbo_script_module_options_t *options);
void turbo_script_instance_options_init(turbo_script_instance_options_t *options);
void turbo_script_call_options_init(turbo_script_call_options_t *options);

TURBO_SCRIPT_C_API turbo_script_status_t
turbo_script_result_create(turbo_script_ctx_t *ctx, turbo_script_result_t **out_result);
TURBO_SCRIPT_C_API void turbo_script_result_reset(turbo_script_result_t *result);
TURBO_SCRIPT_C_API void turbo_script_result_destroy(turbo_script_result_t *result);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_result_get_value(
    const turbo_script_result_t *result, turbo_script_value_view_t *out_value);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_result_get_error(
    const turbo_script_result_t *result, turbo_script_error_info_t *out_error);

turbo_script_status_t
turbo_script_module_compile(turbo_script_ctx_t *ctx, turbo_script_string_view_t source,
                            const turbo_script_module_options_t *options,
                            turbo_script_result_t *result, turbo_script_module_t **out_module);
turbo_script_status_t turbo_script_module_destroy(turbo_script_module_t *module,
                                                  turbo_script_result_t *result);

turbo_script_status_t turbo_script_instance_create(
    turbo_script_module_t *module, const turbo_script_instance_options_t *options,
    turbo_script_result_t *result, turbo_script_instance_t **out_instance);
turbo_script_status_t
turbo_script_instance_destroy(turbo_script_instance_t *instance, turbo_script_result_t *result);
turbo_script_status_t turbo_script_instance_resolve_export(
    turbo_script_instance_t *instance, turbo_script_string_view_t name,
    turbo_script_result_t *result, turbo_script_export_handle_t *out_handle);
turbo_script_status_t turbo_script_instance_get_export_info(
    turbo_script_instance_t *instance, turbo_script_export_handle_t handle,
    turbo_script_result_t *result, turbo_script_export_info_t *out_info);
turbo_script_status_t turbo_script_instance_call(
    turbo_script_instance_t *instance, turbo_script_export_handle_t handle,
    const turbo_script_value_view_t *args, size_t arg_count,
    const turbo_script_call_options_t *options, turbo_script_result_t *result);

TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_context_register_host_function(
    turbo_script_ctx_t *ctx, const turbo_script_host_function_descriptor_t *descriptor,
    turbo_script_host_function_t callback, void *user_data, turbo_script_result_t *result);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_context_unregister_host_function(
    turbo_script_ctx_t *ctx, turbo_script_string_view_t name, turbo_script_result_t *result);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_host_result_set_value(
    turbo_script_host_result_builder_t *builder, const turbo_script_value_view_t *value);
TURBO_SCRIPT_C_API turbo_script_status_t
turbo_script_host_result_set_error(turbo_script_host_result_builder_t *builder, int32_t cause_code,
                                   turbo_script_string_view_t message);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SCRIPT_HOST_API_INTERNAL_H */
