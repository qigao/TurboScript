#include "ts_plugin.h" /* TurboScript canonical plugin-loader fixture. */
#include <limits.h>

#ifndef FIXTURE_PLUGIN_NAME
#define FIXTURE_PLUGIN_NAME "loader_fixture"
#endif

#ifndef FIXTURE_ABI_VERSION
#define FIXTURE_ABI_VERSION SALTS_PLUGIN_ABI_VERSION
#endif

#if defined(FIXTURE_FUNCTION_ONLY)

#if defined(FIXTURE_UNKNOWN_RESULT)
FunctionDecl(value, double, loader_fixture_double,
    (double, value, CMETA_PARAM_IN));
#else
FunctionDeclResult(value, double, CMETA_RESULT_VALUE, loader_fixture_double,
    (double, value, CMETA_PARAM_IN));
#endif
FunctionDeclResult(stateful, double, CMETA_RESULT_VALUE, loader_fixture_stateful,
    (double, value, CMETA_PARAM_IN));
FunctionDeclResult(value, int, CMETA_RESULT_VALUE, loader_fixture_increment,
    (int, value, CMETA_PARAM_IN));
FunctionDeclResult(value, bool, CMETA_RESULT_VALUE, loader_fixture_positive,
    (int, value, CMETA_PARAM_IN));
FunctionDeclResult(value, double, CMETA_RESULT_VALUE, loader_fixture_long_to_double,
    (long, value, CMETA_PARAM_IN));
FunctionDeclResult(value, double, CMETA_RESULT_VALUE, loader_fixture_long_exact,
    (long, value, CMETA_PARAM_IN));
FunctionDeclResult(value, double, CMETA_RESULT_VALUE, loader_fixture_float_to_double,
    (float, value, CMETA_PARAM_IN));
FunctionDeclResult(value, double, CMETA_RESULT_VALUE, loader_fixture_add,
    (double, left, CMETA_PARAM_IN),
    (double, right, CMETA_PARAM_IN));
Function0DeclResult(value, int, CMETA_RESULT_VALUE, loader_fixture_answer);
Function0Decl(value, void, loader_fixture_notify);

double loader_fixture_double(double value) {
  return value * 2.0;
}

double loader_fixture_stateful(double value) {
  return value + 10.0;
}

int loader_fixture_increment(int value) {
  return value + 1;
}

bool loader_fixture_positive(int value) {
  return value > 0;
}

double loader_fixture_long_to_double(long value) {
  return (double)value + 0.5;
}

double loader_fixture_long_exact(long value) {
#if LONG_MAX > 2147483647L
  return value == (long)INT64_C(9007199254740993) ? 1.0 : 0.0;
#else
  return value == LONG_MAX ? 1.0 : 0.0;
#endif
}

double loader_fixture_float_to_double(float value) {
  return (double)value + 0.25;
}

double loader_fixture_add(double left, double right) {
  return left + right;
}

int loader_fixture_answer(void) {
  return 42;
}

void loader_fixture_notify(void) {
}

static bool SALTS_PLUGIN_CALL loader_fixture_double_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  double input;
  if (context != NULL || !return_storage || !params ||
      param_count != 1u || !params[0])
    return false;
  input = *(const double *)params[0];
  *(double *)return_storage = loader_fixture_double(input);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_stateful_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  double input;
  if (context != NULL || !return_storage || !params ||
      param_count != 1u || !params[0])
    return false;
  input = *(const double *)params[0];
  *(double *)return_storage = loader_fixture_stateful(input);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_increment_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int input;
  if (context != NULL || !return_storage || !params ||
      param_count != 1u || !params[0])
    return false;
  input = *(const int *)params[0];
  *(int *)return_storage = loader_fixture_increment(input);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_positive_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int input;
  if (context != NULL || !return_storage || !params ||
      param_count != 1u || !params[0])
    return false;
  input = *(const int *)params[0];
  *(_Bool *)return_storage = loader_fixture_positive(input);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_long_to_double_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  long input;
  if (context != NULL || !return_storage || !params ||
      param_count != 1u || !params[0])
    return false;
  input = *(const long *)params[0];
  *(double *)return_storage = loader_fixture_long_to_double(input);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_long_exact_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  long input;
  if (context != NULL || !return_storage || !params ||
      param_count != 1u || !params[0])
    return false;
  input = *(const long *)params[0];
  *(double *)return_storage = loader_fixture_long_exact(input);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_float_to_double_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  float input;
  if (context != NULL || !return_storage || !params ||
      param_count != 1u || !params[0])
    return false;
  input = *(const float *)params[0];
  *(double *)return_storage = loader_fixture_float_to_double(input);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_add_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  double left;
  double right;
  if (context != NULL || !return_storage || !params ||
      param_count != 2u || !params[0] || !params[1])
    return false;
  left = *(const double *)params[0];
  right = *(const double *)params[1];
  *(double *)return_storage = loader_fixture_add(left, right);
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_answer_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  (void)params;
  if (context != NULL || !return_storage || param_count != 0u)
    return false;
  *(int *)return_storage = loader_fixture_answer();
  return true;
}

static bool SALTS_PLUGIN_CALL loader_fixture_notify_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  (void)params;
  if (context != NULL || return_storage != NULL || param_count != 0u)
    return false;
  loader_fixture_notify();
  return true;
}

static salts_plugin_export loader_fixture_function_exports[10];

static const salts_plugin_manifest loader_fixture_function_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = FIXTURE_PLUGIN_NAME,
    .version = {1u, 0u, 0u},
    .exports = loader_fixture_function_exports,
    .export_count = 10u,
    .self = NULL,
    .start = NULL,
    .request_stop = NULL,
    .is_quiescent = NULL,
    .destroy = NULL,
};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  if (host_abi != SALTS_PLUGIN_ABI_VERSION) return NULL;

  loader_fixture_function_exports[0] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.double",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_double),
          .abi = FunctionAbi(loader_fixture_double),
          .context = NULL,
          .invoke = loader_fixture_double_invoke,
      },
  };
  loader_fixture_function_exports[1] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.stateful",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_stateful),
          .abi = FunctionAbi(loader_fixture_stateful),
          .context = NULL,
          .invoke = loader_fixture_stateful_invoke,
      },
  };
  loader_fixture_function_exports[2] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.increment",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_increment),
          .abi = FunctionAbi(loader_fixture_increment),
          .context = NULL,
          .invoke = loader_fixture_increment_invoke,
      },
  };
  loader_fixture_function_exports[3] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.positive",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_positive),
          .abi = FunctionAbi(loader_fixture_positive),
          .context = NULL,
          .invoke = loader_fixture_positive_invoke,
      },
  };
  loader_fixture_function_exports[4] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.long_to_double",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_long_to_double),
          .abi = FunctionAbi(loader_fixture_long_to_double),
          .context = NULL,
          .invoke = loader_fixture_long_to_double_invoke,
      },
  };
  loader_fixture_function_exports[5] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.long_exact",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_long_exact),
          .abi = FunctionAbi(loader_fixture_long_exact),
          .context = NULL,
          .invoke = loader_fixture_long_exact_invoke,
      },
  };
  loader_fixture_function_exports[6] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.float_to_double",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_float_to_double),
          .abi = FunctionAbi(loader_fixture_float_to_double),
          .context = NULL,
          .invoke = loader_fixture_float_to_double_invoke,
      },
  };
  loader_fixture_function_exports[7] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.add",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_add),
          .abi = FunctionAbi(loader_fixture_add),
          .context = NULL,
          .invoke = loader_fixture_add_invoke,
      },
  };
  loader_fixture_function_exports[8] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.answer",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_answer),
          .abi = FunctionAbi(loader_fixture_answer),
          .context = NULL,
          .invoke = loader_fixture_answer_invoke,
      },
  };
  loader_fixture_function_exports[9] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "loader_function.notify",
      .contract_id = "loader_function.math",
      .value.function = {
          .desc = FunctionMeta(loader_fixture_notify),
          .abi = FunctionAbi(loader_fixture_notify),
          .context = NULL,
          .invoke = loader_fixture_notify_invoke,
      },
  };
  return &loader_fixture_function_manifest;
}

#else

#if defined(FIXTURE_INIT_FAILURE)
static int *loader_fixture_failed_init_unload_count;
#endif

static void *loader_fixture_load(void *self, void *env, void *scratch) {
  (void)self;
#if defined(FIXTURE_INIT_FAILURE)
  loader_fixture_failed_init_unload_count = (int *)scratch;
  (void)env;
  return NULL;
#else
  (void)scratch;
  return env;
#endif
}

static void loader_fixture_unload(void *self, void *instance) {
  (void)self;
#if defined(FIXTURE_INIT_FAILURE)
  if (!instance && loader_fixture_failed_init_unload_count)
    ++*loader_fixture_failed_init_unload_count;
#else
  (void)instance;
#endif
}

static char loader_fixture_token;

static const ts_plugin_module_vtable loader_fixture_vtable = {
    .implementation = FIXTURE_PLUGIN_NAME,
    .capabilities = 0u,
    .load = loader_fixture_load,
    .unload = loader_fixture_unload,
};

static ts_plugin_module loader_fixture_module = {
    .self = &loader_fixture_token,
    .vtable = &loader_fixture_vtable,
};

static const salts_plugin_export loader_fixture_export = {
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = TS_PLUGIN_MODULE_CONTRACT_VERSION,
    .capabilities = 0u,
    .export_id = TS_PLUGIN_MODULE_EXPORT_ID,
    .contract_id = TS_PLUGIN_MODULE_CONTRACT_ID,
    .value.interface = {
        .desc = &ts_plugin_module_interface_meta,
        .value = &loader_fixture_module,
    },
};

static const salts_plugin_manifest loader_fixture_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = FIXTURE_ABI_VERSION,
    .plugin_id = FIXTURE_PLUGIN_NAME,
    .version = {1u, 0u, 0u},
    .exports = &loader_fixture_export,
    .export_count = 1u,
    .self = NULL,
    .start = NULL,
    .request_stop = NULL,
    .is_quiescent = NULL,
    .destroy = NULL,
};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  (void)host_abi;
  return &loader_fixture_manifest;
}

#endif /* FIXTURE_FUNCTION_ONLY */
