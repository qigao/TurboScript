/**
 * @file ts_plugin_loader.c
 * @brief TurboScript host adapter over the canonical Salts plugin registry.
 */
#include "ts_plugin_loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  if (status != SALTS_PLUGIN_OK ||
      salts_plugin_export_require_interface(
          module_export, TS_PLUGIN_MODULE_CONTRACT_ID,
          TS_PLUGIN_MODULE_CONTRACT_VERSION, 0u,
          ts_plugin_module_interface()) != SALTS_PLUGIN_OK) {
    plugin_error_set(error, TS_PLUGIN_ERROR_DESCRIPTOR, TS_PLUGIN_STAGE_ABI,
                     (uint32_t)status, resolved,
                     "plugin does not publish the canonical TurboScript module interface");
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
  registry.impl = NULL;
  free(resolved);

  *out = handle;
  plugin_error_clear(error);
  return TS_PLUGIN_ERROR_NONE;
}

ts_plugin_handle_t *ts_plugin_load(const char *path) {
  ts_plugin_handle_t *handle = NULL;
  if (ts_plugin_load_ex(path, NULL, &handle, NULL) != TS_PLUGIN_ERROR_NONE)
    return NULL;
  return handle;
}

int ts_plugin_init_ex(ts_plugin_handle_t *handle, void *env, void *scratch,
                      ts_plugin_error_t *error) {
  plugin_error_clear(error);
  if (!handle || !handle->manifest || !handle->module ||
      !ts_plugin_module_valid(handle->module))
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_ARGUMENT, 0u, NULL,
                            "validated plugin handle is required");
  if (handle->instance)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                            "plugin is already initialized");

  handle->instance = ts_plugin_module_load(handle->module, env, scratch);
  if (!handle->instance)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INITIALIZE,
                            TS_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                            "plugin initialization returned no instance");
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
