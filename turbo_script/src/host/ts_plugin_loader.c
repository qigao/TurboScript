/**
 * @file ts_plugin_loader.c
 * @brief TurboScript host adapter over the canonical Salts plugin registry.
 */
#include "ts_plugin_loader.h"
#include "ts_plugin_databind.h"
#include "exprtk_runtime_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int plugin_error_set(ts_plugin_error_t *error, ts_plugin_error_code_t code,
                            ts_plugin_error_stage_t stage, uint32_t native_code,
                            const char *path, const char *format, ...);

#define TS_PLUGIN_MAX_SCALAR_PARAMS 16u

typedef enum ts_plugin_scalar_kind_e {
  TS_PLUGIN_SCALAR_UNSUPPORTED = 0,
  TS_PLUGIN_SCALAR_VOID,
  TS_PLUGIN_SCALAR_BOOL,
  TS_PLUGIN_SCALAR_INT,
  TS_PLUGIN_SCALAR_LONG,
  TS_PLUGIN_SCALAR_FLOAT,
  TS_PLUGIN_SCALAR_DOUBLE
} ts_plugin_scalar_kind_t;

typedef union ts_plugin_scalar_storage_u {
  _Bool boolean_value;
  int int_value;
  long long_value;
  float float_value;
  double double_value;
} ts_plugin_scalar_storage_t;

typedef struct ts_plugin_function_binding_s {
  const salts_plugin_export *entry;
  salts_plugin_function_invoke_fn invoke;
  void *context;
  size_t param_count;
  ts_plugin_scalar_kind_t param_kinds[TS_PLUGIN_MAX_SCALAR_PARAMS];
  ts_plugin_scalar_kind_t return_kind;
} ts_plugin_function_binding_t;

static ts_plugin_scalar_kind_t plugin_scalar_kind(
    const cmeta_type_desc *type) {
  if (!type) return TS_PLUGIN_SCALAR_UNSUPPORTED;
  if (cmeta_type_equal(type, &cmeta_type_void))
    return TS_PLUGIN_SCALAR_VOID;
  if (cmeta_type_equal(type, &cmeta_type_bool))
    return TS_PLUGIN_SCALAR_BOOL;
  if (cmeta_type_equal(type, &cmeta_type_int))
    return TS_PLUGIN_SCALAR_INT;
  if (cmeta_type_equal(type, &cmeta_type_long))
    return TS_PLUGIN_SCALAR_LONG;
  if (cmeta_type_equal(type, &cmeta_type_float))
    return TS_PLUGIN_SCALAR_FLOAT;
  if (cmeta_type_equal(type, &cmeta_type_double))
    return TS_PLUGIN_SCALAR_DOUBLE;
  return TS_PLUGIN_SCALAR_UNSUPPORTED;
}

static int plugin_function_scalar_binding(
    const salts_plugin_export *entry,
    ts_plugin_function_binding_t *binding) {
  const cmeta_function_desc *desc;
  const cmeta_function_abi_desc *abi;
  ts_plugin_scalar_kind_t return_kind;

  if (!entry || !binding ||
      entry->kind != SALTS_PLUGIN_EXPORT_FUNCTION)
    return 0;

  desc = entry->value.function.desc;
  abi = entry->value.function.abi;
  if (!desc || !abi || !entry->value.function.invoke ||
      !cmeta_function_desc_valid(desc) ||
      !cmeta_function_abi_desc_valid(abi) ||
      desc->param_count != abi->param_count ||
      desc->param_count > TS_PLUGIN_MAX_SCALAR_PARAMS)
    return 0;

  return_kind = plugin_scalar_kind(desc->return_type);
  if (return_kind == TS_PLUGIN_SCALAR_UNSUPPORTED)
    return 0;
  if (return_kind == TS_PLUGIN_SCALAR_VOID) {
    if (abi->return_carrier != CMETA_ABI_VOID) return 0;
  } else if (abi->return_carrier != CMETA_ABI_SCALAR) {
    return 0;
  }

  memset(binding, 0, sizeof(*binding));
  binding->entry = entry;
  binding->invoke = entry->value.function.invoke;
  binding->context = entry->value.function.context;
  binding->param_count = desc->param_count;
  binding->return_kind = return_kind;

  for (size_t i = 0u; i < desc->param_count; ++i) {
    const cmeta_param_desc *param = cmeta_function_param(desc, i);
    ts_plugin_scalar_kind_t kind;
    if (!param || param->flags != CMETA_PARAM_IN ||
        cmeta_function_param_abi(abi, i) != CMETA_ABI_SCALAR)
      return 0;
    kind = plugin_scalar_kind(param->type);
    if (kind == TS_PLUGIN_SCALAR_UNSUPPORTED ||
        kind == TS_PLUGIN_SCALAR_VOID)
      return 0;
    binding->param_kinds[i] = kind;
  }
  return 1;
}

static int plugin_numeric_value(
    const exprtk_value_t *arg, double *out) {
  if (!arg || !out) return 0;
  if (arg->type == EXPRTK_VAL_NUMBER) {
    *out = arg->data.number;
    return 1;
  }
  if (arg->type == EXPRTK_VAL_INTEGER) {
    *out = (double)arg->data.integer;
    return 1;
  }
  if (arg->type == EXPRTK_VAL_BOOL) {
    *out = arg->data.boolean ? 1.0 : 0.0;
    return 1;
  }
  return 0;
}

static int plugin_scalar_from_exprtk(
    const exprtk_value_t *arg, ts_plugin_scalar_kind_t kind,
    ts_plugin_scalar_storage_t *storage, void **address) {
  double number;

  if (!arg || !storage || !address) return 0;

  switch (kind) {
    case TS_PLUGIN_SCALAR_BOOL:
      if (arg->type == EXPRTK_VAL_BOOL) {
        storage->boolean_value = arg->data.boolean != 0;
      } else if (arg->type == EXPRTK_VAL_INTEGER) {
        if (arg->data.integer != 0 && arg->data.integer != 1) return 0;
        storage->boolean_value = arg->data.integer != 0;
      } else if (arg->type == EXPRTK_VAL_NUMBER) {
        if (!isfinite(arg->data.number) ||
            (arg->data.number != 0.0 && arg->data.number != 1.0))
          return 0;
        storage->boolean_value = arg->data.number != 0.0;
      } else {
        return 0;
      }
      *address = &storage->boolean_value;
      return 1;

    case TS_PLUGIN_SCALAR_INT:
      if (arg->type == EXPRTK_VAL_INTEGER) {
        if (arg->data.integer < (int64_t)INT_MIN ||
            arg->data.integer > (int64_t)INT_MAX)
          return 0;
        storage->int_value = (int)arg->data.integer;
      } else {
        if (!plugin_numeric_value(arg, &number) || !isfinite(number) ||
            number < (double)INT_MIN ||
            number >= (double)INT_MAX + 1.0 ||
            trunc(number) != number)
          return 0;
        storage->int_value = (int)number;
      }
      *address = &storage->int_value;
      return 1;

    case TS_PLUGIN_SCALAR_LONG:
      if (arg->type == EXPRTK_VAL_INTEGER) {
#if LONG_MAX < INT64_MAX
        if (arg->data.integer < (int64_t)LONG_MIN ||
            arg->data.integer > (int64_t)LONG_MAX)
          return 0;
#endif
        storage->long_value = (long)arg->data.integer;
      } else {
        if (!plugin_numeric_value(arg, &number) || !isfinite(number) ||
            number < (double)LONG_MIN ||
            number >= (double)LONG_MAX + 1.0 ||
            trunc(number) != number)
          return 0;
        storage->long_value = (long)number;
      }
      *address = &storage->long_value;
      return 1;

    case TS_PLUGIN_SCALAR_FLOAT:
      if (!plugin_numeric_value(arg, &number) || !isfinite(number) ||
          number < -(double)FLT_MAX || number > (double)FLT_MAX)
        return 0;
      storage->float_value = (float)number;
      *address = &storage->float_value;
      return 1;

    case TS_PLUGIN_SCALAR_DOUBLE:
      if (!plugin_numeric_value(arg, &number) || !isfinite(number))
        return 0;
      storage->double_value = number;
      *address = &storage->double_value;
      return 1;

    default:
      return 0;
  }
}

static void *plugin_scalar_return_storage(
    ts_plugin_scalar_kind_t kind,
    ts_plugin_scalar_storage_t *storage) {
  if (!storage) return NULL;
  switch (kind) {
    case TS_PLUGIN_SCALAR_VOID: return NULL;
    case TS_PLUGIN_SCALAR_BOOL: return &storage->boolean_value;
    case TS_PLUGIN_SCALAR_INT: return &storage->int_value;
    case TS_PLUGIN_SCALAR_LONG: return &storage->long_value;
    case TS_PLUGIN_SCALAR_FLOAT: return &storage->float_value;
    case TS_PLUGIN_SCALAR_DOUBLE: return &storage->double_value;
    default: return NULL;
  }
}

static exprtk_value_t plugin_scalar_to_exprtk(
    ts_plugin_scalar_kind_t kind,
    const ts_plugin_scalar_storage_t *storage) {
  exprtk_value_t value = {0};
  if (!storage && kind != TS_PLUGIN_SCALAR_VOID)
    return exprtk_val_num(0.0);

  switch (kind) {
    case TS_PLUGIN_SCALAR_VOID:
      value.type = EXPRTK_VAL_NULL;
      return value;
    case TS_PLUGIN_SCALAR_BOOL:
      return exprtk_val_bool(storage->boolean_value);
    case TS_PLUGIN_SCALAR_INT:
      return exprtk_val_int((int64_t)storage->int_value);
    case TS_PLUGIN_SCALAR_LONG:
      return exprtk_val_int((int64_t)storage->long_value);
    case TS_PLUGIN_SCALAR_FLOAT:
      return exprtk_val_num((double)storage->float_value);
    case TS_PLUGIN_SCALAR_DOUBLE:
      return exprtk_val_num(storage->double_value);
    default:
      return exprtk_val_num(0.0);
  }
}

static exprtk_value_t plugin_function_scalar_call(
    size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  ts_plugin_function_binding_t *binding =
      (ts_plugin_function_binding_t *)user_data;
  ts_plugin_scalar_storage_t params[TS_PLUGIN_MAX_SCALAR_PARAMS];
  void *param_addresses[TS_PLUGIN_MAX_SCALAR_PARAMS];
  ts_plugin_scalar_storage_t result_storage;
  void *result_address;

  memset(params, 0, sizeof(params));
  memset(param_addresses, 0, sizeof(param_addresses));
  memset(&result_storage, 0, sizeof(result_storage));

  if (!binding || !binding->invoke ||
      argc != binding->param_count ||
      (argc > 0u && !args)) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "invalid canonical plugin function invocation");
    }
    return exprtk_val_num(0.0);
  }

  for (size_t i = 0u; i < argc; ++i) {
    if (!plugin_scalar_from_exprtk(
            &args[i], binding->param_kinds[i],
            &params[i], &param_addresses[i])) {
      if (env) {
        env->aborted = 1;
        snprintf(
            env->error_msg, sizeof(env->error_msg),
            "plugin function '%s' argument %zu does not match canonical scalar type",
            binding->entry && binding->entry->export_id
                ? binding->entry->export_id : "<unknown>",
            i);
      }
      return exprtk_val_num(0.0);
    }
  }

  result_address = plugin_scalar_return_storage(
      binding->return_kind, &result_storage);
  if (binding->return_kind != TS_PLUGIN_SCALAR_VOID &&
      !result_address) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "plugin function '%s' has an unsupported return type",
               binding->entry && binding->entry->export_id
                   ? binding->entry->export_id : "<unknown>");
    }
    return exprtk_val_num(0.0);
  }

  if (!binding->invoke(
          binding->context, result_address,
          argc > 0u ? param_addresses : NULL, argc)) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "plugin function '%s' exact adapter rejected invocation",
               binding->entry && binding->entry->export_id
                   ? binding->entry->export_id : "<unknown>");
    }
    return exprtk_val_num(0.0);
  }

  return plugin_scalar_to_exprtk(
      binding->return_kind, &result_storage);
}

static int plugin_bind_function_exports(
    ts_plugin_handle_t *handle, exprtk_env_t *env,
    ts_plugin_error_t *error) {
  ts_plugin_function_binding_t *bindings = NULL;
  exprtk_native_registration_t *registrations = NULL;
  size_t function_count = 0u;
  size_t registration_index = 0u;
  size_t scalar_count = 0u;
  exprtk_registration_status_t registration_status;

  if (!handle || !handle->manifest || !env)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_ARGUMENT, 0u, NULL,
                            "plugin function binding requires a manifest and environment");

  for (size_t i = 0u; i < handle->manifest->export_count; ++i) {
    if (handle->manifest->exports[i].kind == SALTS_PLUGIN_EXPORT_FUNCTION)
      ++function_count;
  }
  if (function_count == 0u) return TS_PLUGIN_ERROR_NONE;

  bindings = (ts_plugin_function_binding_t *)calloc(
      function_count, sizeof(*bindings));
  registrations = (exprtk_native_registration_t *)calloc(
      function_count, sizeof(*registrations));
  if (!bindings || !registrations) {
    free(registrations);
    free(bindings);
    return plugin_error_set(error, TS_PLUGIN_ERROR_OUT_OF_MEMORY,
                            TS_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                            "out of memory binding canonical plugin functions");
  }

  for (size_t i = 0u; i < handle->manifest->export_count; ++i) {
    const salts_plugin_export *entry = &handle->manifest->exports[i];
    void *databind_binding;
    salts_plugin_status status;
    if (entry->kind != SALTS_PLUGIN_EXPORT_FUNCTION) continue;

    status = salts_plugin_export_require_function(
        entry, entry->contract_id, entry->contract_version, 0u);
    if (status != SALTS_PLUGIN_OK) {
      free(registrations);
      free(bindings);
      return plugin_error_set(
          error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_INITIALIZE,
          (uint32_t)status, NULL,
          "plugin function '%s' failed canonical Function validation",
          entry->export_id ? entry->export_id : "<unnamed>");
    }

    databind_binding =
        ts_plugin_databind_find(handle, entry->export_id);
    if (databind_binding != NULL) {
      registrations[registration_index].name = entry->export_id;
      registrations[registration_index].fn = ts_plugin_databind_call;
      registrations[registration_index].user_data = databind_binding;
      registrations[registration_index].flags =
          EXPRTK_NATIVE_PRESERVE_VALUE_TYPES;
      ++registration_index;
      continue;
    }

    if (!plugin_function_scalar_binding(
            entry, &bindings[scalar_count])) {
      free(registrations);
      free(bindings);
      return plugin_error_set(
          error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_INITIALIZE,
          (uint32_t)status, NULL,
          "plugin function '%s' is neither catalog-backed nor in the canonical finite scalar ABI",
          entry->export_id ? entry->export_id : "<unnamed>");
    }

    registrations[registration_index].name = entry->export_id;
    registrations[registration_index].fn = plugin_function_scalar_call;
    registrations[registration_index].user_data = &bindings[scalar_count];
    registrations[registration_index].flags =
        EXPRTK_NATIVE_PRESERVE_VALUE_TYPES;
    ++scalar_count;
    ++registration_index;
  }

  if (registration_index != function_count) {
    free(registrations);
    free(bindings);
    return plugin_error_set(
        error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_INITIALIZE,
        0u, NULL, "plugin Function registration count is inconsistent");
  }

  registration_status = exprtk_env_register_funcs_checked(
      env, registrations, registration_index);
  free(registrations);
  if (registration_status != EXPRTK_REGISTRATION_OK) {
    free(bindings);
    return plugin_error_set(
        error,
        registration_status == EXPRTK_REGISTRATION_OUT_OF_MEMORY
            ? TS_PLUGIN_ERROR_OUT_OF_MEMORY
            : TS_PLUGIN_ERROR_DESCRIPTOR,
        TS_PLUGIN_STAGE_INITIALIZE, (uint32_t)registration_status, NULL,
        registration_status == EXPRTK_REGISTRATION_CONFLICT
            ? "plugin function export conflicts with an existing script binding"
            : "failed to register canonical plugin function bindings");
  }

  if (scalar_count == 0u) {
    free(bindings);
    bindings = NULL;
  }

  handle->function_bindings = bindings;
  handle->function_binding_count = scalar_count;
  return TS_PLUGIN_ERROR_NONE;
}

static void plugin_error_clear(ts_plugin_error_t *error) {
  if (error) memset(error, 0, sizeof(*error));
}

static int plugin_error_set(ts_plugin_error_t *error, ts_plugin_error_code_t code,
                            ts_plugin_error_stage_t stage, uint32_t native_code,
                            const char *path, const char *format, ...) {
  if (error) {
    va_list args;
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->stage = stage;
    error->native_code = native_code;
    if (path)
      snprintf(error->attempted_path, sizeof(error->attempted_path), "%s", path);
    if (format) {
      va_start(args, format);
      vsnprintf(error->message, sizeof(error->message), format, args);
      va_end(args);
    }
  }
  return code;
}

static char *plugin_strdup(const char *text) {
  size_t length;
  char *copy;
  if (!text) return NULL;
  length = strlen(text);
  if (length == SIZE_MAX) return NULL;
  copy = (char *)malloc(length + 1u);
  if (!copy) return NULL;
  memcpy(copy, text, length + 1u);
  return copy;
}

static char *plugin_join_path(const char *directory, const char *leaf) {
  size_t directory_len;
  size_t leaf_len;
  char separator =
#if defined(_WIN32)
      '\\';
#else
      '/';
#endif
  char *path;

  if (!directory || !leaf) return NULL;
  directory_len = strlen(directory);
  leaf_len = strlen(leaf);
  if (directory_len > SIZE_MAX - leaf_len - 2u) return NULL;
  path = (char *)malloc(directory_len + leaf_len + 2u);
  if (!path) return NULL;
  memcpy(path, directory, directory_len);
  path[directory_len] = separator;
  memcpy(path + directory_len + 1u, leaf, leaf_len + 1u);
  return path;
}

#if defined(_WIN32)
#include <windows.h>

static int plugin_has_path_separator(const char *path) {
  return path && (strchr(path, '\\') || strchr(path, '/') || strchr(path, ':'));
}

static wchar_t *plugin_utf8_to_wide(const char *text) {
  int length;
  wchar_t *wide;
  if (!text || !*text) return NULL;
  length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (length <= 0) return NULL;
  wide = (wchar_t *)malloc((size_t)length * sizeof(*wide));
  if (!wide) return NULL;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
    free(wide);
    return NULL;
  }
  return wide;
}

static char *plugin_wide_to_utf8(const wchar_t *wide) {
  int length;
  char *text;
  if (!wide || !*wide) return NULL;
  length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (length <= 0) return NULL;
  text = (char *)malloc((size_t)length);
  if (!text) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, length, NULL, NULL) <= 0) {
    free(text);
    return NULL;
  }
  return text;
}

static char *plugin_executable_dir(void) {
  DWORD capacity = 256u;
  wchar_t *buffer = NULL;

  for (;;) {
    DWORD length;
    wchar_t *slash;
    wchar_t *grown = (wchar_t *)realloc(buffer, (size_t)capacity * sizeof(*buffer));
    if (!grown) {
      free(buffer);
      return NULL;
    }
    buffer = grown;
    SetLastError(ERROR_SUCCESS);
    length = GetModuleFileNameW(NULL, buffer, capacity);
    if (length == 0u) {
      free(buffer);
      return NULL;
    }
    if (length < capacity && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      char *result;
      slash = wcsrchr(buffer, L'\\');
      if (!slash) slash = wcsrchr(buffer, L'/');
      if (!slash) {
        free(buffer);
        return NULL;
      }
      *slash = L'\0';
      result = plugin_wide_to_utf8(buffer);
      free(buffer);
      return result;
    }
    if (capacity > UINT32_MAX / 2u) {
      free(buffer);
      return NULL;
    }
    capacity *= 2u;
  }
}

static int plugin_path_exists(const char *path) {
  DWORD attrs;
  wchar_t *wide = plugin_utf8_to_wide(path);
  if (!wide) return 0;
  attrs = GetFileAttributesW(wide);
  free(wide);
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

#else

#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static int plugin_has_path_separator(const char *path) {
  return path && strchr(path, '/');
}

static char *plugin_executable_path(void) {
#if defined(__APPLE__)
  uint32_t size = 0u;
  char *path;
  if (_NSGetExecutablePath(NULL, &size) == 0 || size == 0u) return NULL;
  path = (char *)malloc(size);
  if (!path) return NULL;
  if (_NSGetExecutablePath(path, &size) != 0) {
    free(path);
    return NULL;
  }
  return path;
#elif defined(__linux__)
  size_t capacity = 256u;
  char *path = NULL;
  for (;;) {
    ssize_t length;
    char *grown = (char *)realloc(path, capacity);
    if (!grown) {
      free(path);
      return NULL;
    }
    path = grown;
    length = readlink("/proc/self/exe", path, capacity - 1u);
    if (length < 0) {
      free(path);
      return NULL;
    }
    if ((size_t)length < capacity - 1u) {
      path[length] = '\0';
      return path;
    }
    if (capacity > SIZE_MAX / 2u) {
      free(path);
      return NULL;
    }
    capacity *= 2u;
  }
#else
  return NULL;
#endif
}

static char *plugin_executable_dir(void) {
  char *path = plugin_executable_path();
  char *slash;
  if (!path) return NULL;
  slash = strrchr(path, '/');
  if (!slash) {
    free(path);
    return NULL;
  }
  *slash = '\0';
  return path;
}

static int plugin_path_exists(const char *path) {
  return path && access(path, R_OK) == 0;
}
#endif

static char *plugin_resolve_path(const char *path, ts_plugin_error_t *error) {
  char *exe_dir;
  char *plugins_dir;
  char *candidate;
  const char *root;

  if (!path || !*path) {
    plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                     TS_PLUGIN_STAGE_ARGUMENT, 0u, path,
                     "plugin path is required");
    return NULL;
  }

  if (plugin_has_path_separator(path)) {
    if (plugin_path_exists(path)) return plugin_strdup(path);
    plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN, 0u,
                     path, "plugin file does not exist");
    return NULL;
  }

  exe_dir = plugin_executable_dir();
  if (!exe_dir) {
    plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN, 0u,
                     path, "failed to determine executable directory");
    return NULL;
  }

  plugins_dir = plugin_join_path(exe_dir, "plugins");
  candidate = plugins_dir ? plugin_join_path(plugins_dir, path) : NULL;
  free(plugins_dir);
  if (candidate && plugin_path_exists(candidate)) {
    free(exe_dir);
    return candidate;
  }
  free(candidate);

  root = getenv("TURBOSCRIPT_ROOT");
  if (root && *root) {
    char *bin_dir = plugin_join_path(root, "bin");
    plugins_dir = bin_dir ? plugin_join_path(bin_dir, "plugins") : NULL;
    free(bin_dir);
    candidate = plugins_dir ? plugin_join_path(plugins_dir, path) : NULL;
    free(plugins_dir);
    if (candidate && plugin_path_exists(candidate)) {
      free(exe_dir);
      return candidate;
    }
    free(candidate);
  }

  candidate = plugin_join_path(exe_dir, path);
  free(exe_dir);
  if (candidate && plugin_path_exists(candidate)) return candidate;

  plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN, 0u,
                   candidate ? candidate : path,
                   "plugin was not found in executable/package plugin locations");
  free(candidate);
  return NULL;
}

static int plugin_error_from_salts(ts_plugin_error_t *error,
                                   salts_plugin_status status,
                                   const char *path) {
  ts_plugin_error_code_t code = TS_PLUGIN_ERROR_DESCRIPTOR;
  ts_plugin_error_stage_t stage = TS_PLUGIN_STAGE_ABI;
  const char *detail = salts_plugin_status_string(status);

  switch (status) {
    case SALTS_PLUGIN_LOAD_FAILED:
      code = TS_PLUGIN_ERROR_OPEN;
      stage = TS_PLUGIN_STAGE_OPEN;
      break;
    case SALTS_PLUGIN_QUERY_MISSING:
      code = TS_PLUGIN_ERROR_SYMBOL;
      stage = TS_PLUGIN_STAGE_SYMBOL;
      break;
    case SALTS_PLUGIN_QUERY_REJECTED:
    case SALTS_PLUGIN_UNSUPPORTED_ABI:
      code = TS_PLUGIN_ERROR_ABI;
      stage = TS_PLUGIN_STAGE_ABI;
      break;
    case SALTS_PLUGIN_ALLOCATION_FAILED:
      code = TS_PLUGIN_ERROR_OUT_OF_MEMORY;
      stage = TS_PLUGIN_STAGE_OPEN;
      break;
    default:
      code = TS_PLUGIN_ERROR_DESCRIPTOR;
      stage = TS_PLUGIN_STAGE_ABI;
      break;
  }

  if (status == SALTS_PLUGIN_QUERY_MISSING) {
    return plugin_error_set(error, code, stage, (uint32_t)status, path,
                            "required symbol '%s' was not found",
                            SALTS_PLUGIN_QUERY_SYMBOL);
  }
  if (status == SALTS_PLUGIN_UNSUPPORTED_ABI ||
      status == SALTS_PLUGIN_QUERY_REJECTED) {
    return plugin_error_set(error, code, stage, (uint32_t)status, path,
                            "plugin ABI mismatch: %s", detail);
  }
  return plugin_error_set(error, code, stage, (uint32_t)status, path,
                          "Salts plugin admission failed: %s", detail);
}

static void plugin_registry_cleanup_loaded(salts_plugin_registry *registry,
                                           salts_plugin_ref ref,
                                           salts_plugin_lease *lease,
                                           int started) {
  bool quiescent = false;

  if (!registry || !registry->impl) return;
  if (lease && salts_plugin_lease_valid(*lease))
    (void)salts_plugin_registry_release(registry, lease);

  if (salts_plugin_ref_valid(ref)) {
    if (started) {
      salts_plugin_status status =
          salts_plugin_registry_request_stop(registry, ref);
      if (status == SALTS_PLUGIN_OK || status == SALTS_PLUGIN_ALREADY)
        (void)salts_plugin_registry_poll_quiescent(
            registry, ref, &quiescent);
      if (quiescent)
        (void)salts_plugin_registry_unload(registry, ref);
    } else {
      (void)salts_plugin_registry_unload(registry, ref);
    }
  }
  (void)salts_plugin_registry_destroy(registry);
}

int ts_plugin_load_ex(const char *path, const char *expected_name,
                      ts_plugin_handle_t **out, ts_plugin_error_t *error) {
  char *resolved = NULL;
  salts_plugin_registry registry = {0};
  salts_plugin_registry_config config = {.capacity = 1u};
  salts_plugin_ref ref = {0};
  salts_plugin_lease lease = {0};
  const salts_plugin_manifest *manifest = NULL;
  const salts_plugin_export *module_export = NULL;
  ts_plugin_module *module = NULL;
  salts_plugin_status status;
  ts_plugin_handle_t *handle = NULL;
  int started = 0;

  plugin_error_clear(error);
  if (out) *out = NULL;
  if (!path || !*path || !out)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_ARGUMENT, 0u, path,
                            "plugin path and output handle are required");

  resolved = plugin_resolve_path(path, error);
  if (!resolved) return error ? error->code : TS_PLUGIN_ERROR_OPEN;

  status = salts_plugin_registry_init(&registry, &config);
  if (status != SALTS_PLUGIN_OK) {
    free(resolved);
    return plugin_error_from_salts(error, status, path);
  }

  status = salts_plugin_registry_load(&registry, resolved, &ref);
  if (status != SALTS_PLUGIN_OK) {
    int result = plugin_error_from_salts(error, status, resolved);
    free(resolved);
    (void)salts_plugin_registry_destroy(&registry);
    return result;
  }

  status = salts_plugin_registry_start(&registry, ref);
  if (status != SALTS_PLUGIN_OK) {
    int result = plugin_error_from_salts(error, status, resolved);
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, 0);
    return result;
  }
  started = 1;

  status = salts_plugin_registry_acquire(&registry, ref, &lease, &manifest);
  if (status != SALTS_PLUGIN_OK) {
    int result = plugin_error_from_salts(error, status, resolved);
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return result;
  }

  if (manifest->self != NULL || manifest->start != NULL ||
      manifest->request_stop != NULL || manifest->is_quiescent != NULL ||
      manifest->destroy != NULL) {
    plugin_error_set(error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_ABI,
                     0u, resolved,
                     "TurboScript module plugins must use passive Salts DSO lifecycle");
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return TS_PLUGIN_ERROR_DESCRIPTOR;
  }

  if (expected_name && strcmp(manifest->plugin_id, expected_name) != 0) {
    plugin_error_set(error, TS_PLUGIN_ERROR_NAME, TS_PLUGIN_STAGE_ABI, 0u,
                     resolved,
                     "plugin name mismatch: expected '%s', got '%s'",
                     expected_name, manifest->plugin_id);
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return TS_PLUGIN_ERROR_NAME;
  }

  status = salts_plugin_manifest_find_export(
      manifest, TS_PLUGIN_MODULE_EXPORT_ID, &module_export);
  if (status == SALTS_PLUGIN_OK) {
    if (salts_plugin_export_require_interface(
            module_export, TS_PLUGIN_MODULE_CONTRACT_ID,
            TS_PLUGIN_MODULE_CONTRACT_VERSION, 0u,
            ts_plugin_module_interface()) != SALTS_PLUGIN_OK) {
      plugin_error_set(error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_ABI,
                       (uint32_t)status, resolved,
                       "TurboScript module interface export is invalid");
      free(resolved);
      plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
      return TS_PLUGIN_ERROR_DESCRIPTOR;
    }

    module = (ts_plugin_module *)module_export->value.interface.value;
    if (!ts_plugin_module_valid(module)) {
      plugin_error_set(error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_ABI,
                       0u, resolved,
                       "TurboScript module interface value is invalid");
      free(resolved);
      plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
      return TS_PLUGIN_ERROR_DESCRIPTOR;
    }
  } else if (status == SALTS_PLUGIN_UNKNOWN_EXPORT) {
    size_t function_count = 0u;
    module_export = NULL;
    for (size_t i = 0u; i < manifest->export_count; ++i) {
      if (manifest->exports[i].kind == SALTS_PLUGIN_EXPORT_FUNCTION)
        ++function_count;
    }
    if (function_count == 0u) {
      plugin_error_set(
          error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_ABI,
          (uint32_t)status, resolved,
          "plugin publishes neither canonical Function exports nor the TurboScript module interface");
      free(resolved);
      plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
      return TS_PLUGIN_ERROR_DESCRIPTOR;
    }
  } else {
    plugin_error_set(error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_ABI,
                     (uint32_t)status, resolved,
                     "failed to inspect canonical plugin exports");
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return TS_PLUGIN_ERROR_DESCRIPTOR;
  }

  handle = (ts_plugin_handle_t *)calloc(1, sizeof(*handle));
  if (!handle) {
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return plugin_error_set(error, TS_PLUGIN_ERROR_OUT_OF_MEMORY,
                            TS_PLUGIN_STAGE_OPEN, 0u, path,
                            "out of memory while creating plugin handle");
  }

  handle->registry = registry;
  handle->plugin_ref = ref;
  handle->lease = lease;
  handle->manifest = manifest;
  handle->module_export = module_export;
  handle->module = module;
  handle->instance = NULL;
  handle->function_bindings = NULL;
  handle->function_binding_count = 0u;
  handle->databind_bindings = NULL;
  handle->databind_binding_count = 0u;
  handle->databind_codec = NULL;
  handle->initialized = 0;
  registry.impl = NULL;
  free(resolved);

  *out = handle;
  plugin_error_clear(error);
  return TS_PLUGIN_ERROR_NONE;
}

int ts_plugin_find_bound_function(
    const ts_plugin_handle_t *handle, const char *export_id,
    ts_plugin_function_view_t *out) {
  const ts_plugin_function_binding_t *bindings;

  if (out) memset(out, 0, sizeof(*out));
  if (!handle || !export_id || !*export_id || !out ||
      !handle->function_bindings || handle->function_binding_count == 0u)
    return 0;

  bindings =
      (const ts_plugin_function_binding_t *)handle->function_bindings;
  for (size_t i = 0u; i < handle->function_binding_count; ++i) {
    const salts_plugin_export *entry = bindings[i].entry;
    if (entry && entry->export_id &&
        strcmp(entry->export_id, export_id) == 0) {
      out->entry = entry;
      out->invoke = bindings[i].invoke;
      out->context = bindings[i].context;
      return 1;
    }
  }
  return 0;
}

ts_plugin_handle_t *ts_plugin_load(const char *path) {
  ts_plugin_handle_t *handle = NULL;
  if (ts_plugin_load_ex(path, NULL, &handle, NULL) != TS_PLUGIN_ERROR_NONE)
    return NULL;
  return handle;
}

int ts_plugin_init_ex(ts_plugin_handle_t *handle, void *env, void *scratch,
                      ts_plugin_error_t *error) {
  int bind_status;

  plugin_error_clear(error);
  if (!handle || !handle->manifest || !env)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_ARGUMENT, 0u, NULL,
                            "validated plugin handle and target environment are required");
  if (handle->initialized)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                            "plugin is already initialized");

  if (handle->module && ts_plugin_module_valid(handle->module)) {
    handle->instance = ts_plugin_module_load(handle->module, env, scratch);
    if (!handle->instance)
      return plugin_error_set(error, TS_PLUGIN_ERROR_INITIALIZE,
                              TS_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                              "plugin module initialization returned no instance");
  }

  {
    char databind_error[TS_PLUGIN_ERROR_MESSAGE_CAPACITY] = {0};
    if (!ts_plugin_databind_prepare(
            handle, databind_error, sizeof(databind_error))) {
      if (handle->instance && handle->module &&
          ts_plugin_module_valid(handle->module)) {
        ts_plugin_module_unload(handle->module, handle->instance);
        handle->instance = NULL;
      }
      return plugin_error_set(
          error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_INITIALIZE,
          0u, NULL, "%s",
          databind_error[0]
              ? databind_error
              : "DataBind Service catalog binding failed");
    }
  }

  bind_status = plugin_bind_function_exports(
      handle, (exprtk_env_t *)env, error);
  if (bind_status != TS_PLUGIN_ERROR_NONE) {
    ts_plugin_databind_clear(handle);
    if (handle->instance && handle->module &&
        ts_plugin_module_valid(handle->module)) {
      ts_plugin_module_unload(handle->module, handle->instance);
      handle->instance = NULL;
    }
    return bind_status;
  }

  handle->initialized = 1;
  return TS_PLUGIN_ERROR_NONE;
}

int ts_plugin_init(ts_plugin_handle_t *handle, void *env, void *scratch) {
  return ts_plugin_init_ex(handle, env, scratch, NULL) ==
                 TS_PLUGIN_ERROR_NONE
             ? 0
             : -1;
}

void ts_plugin_unload(ts_plugin_handle_t *handle) {
  bool quiescent = false;

  if (!handle) return;

  if (handle->instance && handle->module &&
      ts_plugin_module_valid(handle->module)) {
    ts_plugin_module_unload(handle->module, handle->instance);
    handle->instance = NULL;
  }

  ts_plugin_databind_clear(handle);
  free(handle->function_bindings);
  handle->function_bindings = NULL;
  handle->function_binding_count = 0u;
  handle->initialized = 0;

  if (salts_plugin_lease_valid(handle->lease))
    (void)salts_plugin_registry_release(&handle->registry, &handle->lease);

  if (salts_plugin_ref_valid(handle->plugin_ref)) {
    salts_plugin_status status =
        salts_plugin_registry_request_stop(&handle->registry,
                                           handle->plugin_ref);
    if (status == SALTS_PLUGIN_OK || status == SALTS_PLUGIN_ALREADY)
      (void)salts_plugin_registry_poll_quiescent(
          &handle->registry, handle->plugin_ref, &quiescent);
    if (quiescent)
      (void)salts_plugin_registry_unload(&handle->registry,
                                         handle->plugin_ref);
  }

  (void)salts_plugin_registry_destroy(&handle->registry);
  free(handle);
}
