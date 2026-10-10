/**
 * @file ts_plugin_loader.c
 * @brief TurboScript host cross-platform plugin loader implementation.
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

/* ── Platform dlopen shim ─────────────────────────────────────────── */
#ifdef _WIN32
  #include <windows.h>

static int has_path_separator_win(const char *path) {
  return path && (strchr(path, '\\') || strchr(path, '/') || strchr(path, ':'));
}

static wchar_t *utf8_to_wide(const char *text) {
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

static wchar_t *get_exe_dir_win(void) {
  DWORD capacity = 256;
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
    if (length == 0) {
      free(buffer);
      return NULL;
    }
    if (length < capacity && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      slash = wcsrchr(buffer, L'\\');
      if (!slash) slash = wcsrchr(buffer, L'/');
      if (!slash) {
        free(buffer);
        return NULL;
      }
      *slash = L'\0';
      return buffer;
    }
    if (capacity > UINT32_MAX / 2U) {
      free(buffer);
      return NULL;
    }
    capacity *= 2U;
  }
}

static wchar_t *join_path_win(const wchar_t *directory, const wchar_t *leaf) {
  size_t directory_len;
  size_t leaf_len;
  wchar_t *path;

  if (!directory || !leaf) return NULL;
  directory_len = wcslen(directory);
  leaf_len = wcslen(leaf);
  if (directory_len > (SIZE_MAX / sizeof(*path)) - leaf_len - 2U) return NULL;
  path = (wchar_t *)malloc((directory_len + leaf_len + 2U) * sizeof(*path));
  if (!path) return NULL;
  memcpy(path, directory, directory_len * sizeof(*path));
  path[directory_len] = L'\\';
  memcpy(path + directory_len + 1U, leaf, (leaf_len + 1U) * sizeof(*path));
  return path;
}

static wchar_t *package_plugin_path_win(const wchar_t *leaf) {
  const char *root;
  wchar_t *wide_root;
  wchar_t *plugins_dir;
  wchar_t *path;

  if (!leaf || !(root = getenv("TURBOSCRIPT_ROOT")) || !*root) return NULL;
  wide_root = utf8_to_wide(root);
  if (!wide_root) return NULL;
  plugins_dir = join_path_win(wide_root, L"bin\\plugins");
  free(wide_root);
  if (!plugins_dir) return NULL;
  path = join_path_win(plugins_dir, leaf);
  free(plugins_dir);
  return path;
}

static DLL_DIRECTORY_COOKIE add_package_dll_directory(const char *root_name) {
  const char *root;
  wchar_t *wide_root;
  wchar_t *bin_dir;
  DLL_DIRECTORY_COOKIE cookie;

  if (!root_name || !(root = getenv(root_name)) || !*root) return NULL;
  wide_root = utf8_to_wide(root);
  if (!wide_root) return NULL;
  bin_dir = join_path_win(wide_root, L"bin");
  free(wide_root);
  if (!bin_dir) return NULL;
  cookie = AddDllDirectory(bin_dir);
  free(bin_dir);
  return cookie;
}

static DLL_DIRECTORY_COOKIE add_runtime_dll_directory(const char *directory_name) {
  const char *directory;
  wchar_t *wide_directory;
  DLL_DIRECTORY_COOKIE cookie;

  if (!directory_name || !(directory = getenv(directory_name)) || !*directory) return NULL;
  wide_directory = utf8_to_wide(directory);
  if (!wide_directory) return NULL;
  cookie = AddDllDirectory(wide_directory);
  free(wide_directory);
  return cookie;
}

static void *open_exact_win(const wchar_t *path, const wchar_t *dependency_dir,
                            DWORD *native_error) {
  DWORD length;
  wchar_t *absolute_path;
  DLL_DIRECTORY_COOKIE dependency_cookie;
  DLL_DIRECTORY_COOKIE salts_cookie;
  DLL_DIRECTORY_COOKIE salts_utils_cookie;
  DLL_DIRECTORY_COOKIE chttp_cookie;
  DLL_DIRECTORY_COOKIE vcpkg_cookie;
  HMODULE module;
  const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                      LOAD_LIBRARY_SEARCH_USER_DIRS |
                      LOAD_LIBRARY_SEARCH_SYSTEM32;

  if (native_error) *native_error = ERROR_SUCCESS;
  if (!path || !*path) {
    if (native_error) *native_error = ERROR_INVALID_PARAMETER;
    return NULL;
  }
  dependency_cookie = AddDllDirectory(dependency_dir);
  if (!dependency_cookie) {
    if (native_error) *native_error = GetLastError();
    return NULL;
  }
  /* LoadLibraryExW intentionally excludes PATH.  Register the explicitly
   * configured package roots instead, so plugins can resolve their Salts
   * runtime DLLs without weakening the plugin search policy. */
  salts_cookie = add_package_dll_directory("SALTS_ROOT");
  salts_utils_cookie = add_package_dll_directory("SALTS_UTILS_ROOT");
  chttp_cookie = add_package_dll_directory("CHTTP_ROOT");
  vcpkg_cookie = add_runtime_dll_directory("VCPKG_RUNTIME_BIN");
  length = GetFullPathNameW(path, 0, NULL, NULL);
  if (length == 0) {
    if (native_error) *native_error = GetLastError();
    if (vcpkg_cookie) RemoveDllDirectory(vcpkg_cookie);
    if (chttp_cookie) RemoveDllDirectory(chttp_cookie);
    if (salts_utils_cookie) RemoveDllDirectory(salts_utils_cookie);
    if (salts_cookie) RemoveDllDirectory(salts_cookie);
    RemoveDllDirectory(dependency_cookie);
    return NULL;
  }
  absolute_path = (wchar_t *)malloc((size_t)length * sizeof(*absolute_path));
  if (!absolute_path) {
    if (native_error) *native_error = ERROR_NOT_ENOUGH_MEMORY;
    if (vcpkg_cookie) RemoveDllDirectory(vcpkg_cookie);
    if (chttp_cookie) RemoveDllDirectory(chttp_cookie);
    if (salts_utils_cookie) RemoveDllDirectory(salts_utils_cookie);
    if (salts_cookie) RemoveDllDirectory(salts_cookie);
    RemoveDllDirectory(dependency_cookie);
    return NULL;
  }
  if (GetFullPathNameW(path, length, absolute_path, NULL) == 0) {
    if (native_error) *native_error = GetLastError();
    free(absolute_path);
    if (vcpkg_cookie) RemoveDllDirectory(vcpkg_cookie);
    if (chttp_cookie) RemoveDllDirectory(chttp_cookie);
    if (salts_utils_cookie) RemoveDllDirectory(salts_utils_cookie);
    if (salts_cookie) RemoveDllDirectory(salts_cookie);
    RemoveDllDirectory(dependency_cookie);
    return NULL;
  }
  module = LoadLibraryExW(absolute_path, NULL, flags);
  if (!module && native_error) *native_error = GetLastError();
  if (vcpkg_cookie) RemoveDllDirectory(vcpkg_cookie);
  if (chttp_cookie) RemoveDllDirectory(chttp_cookie);
  if (salts_utils_cookie) RemoveDllDirectory(salts_utils_cookie);
  if (salts_cookie) RemoveDllDirectory(salts_cookie);
  RemoveDllDirectory(dependency_cookie);
  free(absolute_path);
  return (void *)module;
}

static void wide_path_to_utf8(const wchar_t *path, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!path) return;
  if (WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)out_size, NULL, NULL) <= 0)
    out[0] = '\0';
}

static void *pl_open_native(const char *path, ts_plugin_error_t *error) {
  void *module = NULL;
  DWORD native_error = ERROR_SUCCESS;
  wchar_t *wide_path;
  wchar_t *exe_dir;
  wchar_t *plugins_dir;
  wchar_t *candidate;
  char attempted_path[TS_PLUGIN_ERROR_PATH_CAPACITY] = {0};

  wide_path = utf8_to_wide(path);
  if (!wide_path) {
    plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT, TS_PLUGIN_STAGE_OPEN,
                     GetLastError(), path, "plugin path is not valid UTF-8");
    return NULL;
  }
  exe_dir = get_exe_dir_win();
  if (!exe_dir) {
    free(wide_path);
    plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN,
                     GetLastError(), path, "failed to determine executable directory");
    return NULL;
  }
  if (has_path_separator_win(path)) {
    module = open_exact_win(wide_path, exe_dir, &native_error);
    free(exe_dir);
    free(wide_path);
    if (!module)
      plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN,
                       native_error, path, "LoadLibraryExW failed with Win32 error %lu",
                       (unsigned long)native_error);
    return module;
  }
  plugins_dir = join_path_win(exe_dir, L"plugins");
  candidate = plugins_dir ? join_path_win(plugins_dir, wide_path) : NULL;
  if (candidate) module = open_exact_win(candidate, exe_dir, &native_error);
  free(candidate);
  free(plugins_dir);

  if (!module) {
    candidate = package_plugin_path_win(wide_path);
    if (candidate) module = open_exact_win(candidate, exe_dir, &native_error);
    free(candidate);
  }

  if (!module) {
    candidate = join_path_win(exe_dir, wide_path);
    if (candidate) module = open_exact_win(candidate, exe_dir, &native_error);
    wide_path_to_utf8(candidate, attempted_path, sizeof(attempted_path));
    free(candidate);
  }
  free(exe_dir);
  free(wide_path);
  if (!module)
    plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN,
                     native_error, attempted_path[0] ? attempted_path : path,
                     "LoadLibraryExW failed with Win32 error %lu",
                     (unsigned long)native_error);
  return module;
}

static void pl_dlclose(void *h) { if (!FreeLibrary((HMODULE)h)) abort(); }

static void *pl_dlopen(const char *path, char **resolved, ts_plugin_error_t *error) {
  wchar_t wide[CMETA_PLUGIN_PATH_MAX + 1u];
  DWORD length;
  void *module = pl_open_native(path, error);
  if (!module) return NULL;
  *resolved = (char *)malloc(CMETA_PLUGIN_PATH_MAX + 1u);
  length = GetModuleFileNameW((HMODULE)module, wide, CMETA_PLUGIN_PATH_MAX + 1u);
  if (!*resolved || !length || length > CMETA_PLUGIN_PATH_MAX ||
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, *resolved,
          CMETA_PLUGIN_PATH_MAX + 1u, NULL, NULL) <= 0) {
    free(*resolved);
    *resolved = NULL;
    pl_dlclose(module);
    plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN,
        0, path, "cannot resolve plugin path within Salts Plugin path capacity");
    return NULL;
  }
  return module;
}
#else
  #include <dlfcn.h>
  #include <limits.h>
  #include <unistd.h>
#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif

static int has_path_separator(const char *path) {
  return path && strchr(path, '/');
}

static char *get_exe_path_posix(void) {
#if defined(__APPLE__)
  uint32_t size = 0;
  char *path;
  if (_NSGetExecutablePath(NULL, &size) == 0 || size == 0) return NULL;
  path = (char *)malloc(size);
  if (!path) return NULL;
  if (_NSGetExecutablePath(path, &size) != 0) {
    free(path);
    return NULL;
  }
  return path;
#elif defined(__linux__)
  size_t capacity = 256;
  char *path = NULL;
  for (;;) {
    ssize_t length;
    char *grown = (char *)realloc(path, capacity);
    if (!grown) {
      free(path);
      return NULL;
    }
    path = grown;
    length = readlink("/proc/self/exe", path, capacity - 1U);
    if (length < 0) {
      free(path);
      return NULL;
    }
    if ((size_t)length < capacity - 1U) {
      path[length] = '\0';
      return path;
    }
    if (capacity > SIZE_MAX / 2U) {
      free(path);
      return NULL;
    }
    capacity *= 2U;
  }
#else
  return NULL;
#endif
}

static char *get_exe_dir_posix(void) {
  char *path = get_exe_path_posix();
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

static char *join_path_posix(const char *directory, const char *leaf) {
  size_t directory_len;
  size_t leaf_len;
  char *path;
  if (!directory || !leaf) return NULL;
  directory_len = strlen(directory);
  leaf_len = strlen(leaf);
  if (directory_len > SIZE_MAX - leaf_len - 2U) return NULL;
  path = (char *)malloc(directory_len + leaf_len + 2U);
  if (!path) return NULL;
  memcpy(path, directory, directory_len);
  path[directory_len] = '/';
  memcpy(path + directory_len + 1U, leaf, leaf_len + 1U);
  return path;
}

static void *open_exact_posix(const char *path, char **resolved) {
  void *module;
  (void)dlerror();
  module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (module) {
    *resolved = realpath(path, NULL);
    if (!*resolved) {
      if (dlclose(module) != 0) abort();
      return NULL;
    }
  }
  return module;
}

static void *pl_dlopen(const char *path, char **resolved, ts_plugin_error_t *error) {
  void *module = NULL;
  const char *detail = NULL;
  char *exe_dir;
  char *plugins_dir;
  char *candidate;

  if (has_path_separator(path)) {
    module = open_exact_posix(path, resolved);
    if (!module) {
      detail = dlerror();
      plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN, 0,
                       path, "%s", detail ? detail : "dlopen failed");
    }
    return module;
  }
  exe_dir = get_exe_dir_posix();
  if (!exe_dir) {
    plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN, 0,
                     path, "failed to determine executable directory");
    return NULL;
  }
  plugins_dir = join_path_posix(exe_dir, "plugins");
  candidate = plugins_dir ? join_path_posix(plugins_dir, path) : NULL;
  if (candidate) module = open_exact_posix(candidate, resolved);
  free(candidate);
  free(plugins_dir);
  if (!module) {
    candidate = join_path_posix(exe_dir, path);
    if (candidate) module = open_exact_posix(candidate, resolved);
    if (!module) {
      detail = dlerror();
      plugin_error_set(error, TS_PLUGIN_ERROR_OPEN, TS_PLUGIN_STAGE_OPEN, 0,
                       candidate ? candidate : path, "%s",
                       detail ? detail : "dlopen failed");
    }
    free(candidate);
  }
  free(exe_dir);
  return module;
}

static void pl_dlclose(void *h) { if (dlclose(h) != 0) abort(); }
#endif

static int plugin_salts_error(ts_plugin_error_t *error, cmeta_plugin_status status,
                              ts_plugin_error_stage_t stage, const char *path) {
  ts_plugin_error_code_t code = TS_PLUGIN_ERROR_CONTRACT;
  if (status == CMETA_PLUGIN_QUERY_MISSING) code = TS_PLUGIN_ERROR_SYMBOL;
  else if (status == CMETA_PLUGIN_UNSUPPORTED_ABI || status == CMETA_PLUGIN_QUERY_REJECTED)
    code = TS_PLUGIN_ERROR_ABI;
  else if (status == CMETA_PLUGIN_ALLOCATION_FAILED) code = TS_PLUGIN_ERROR_OUT_OF_MEMORY;
  else if (stage == TS_PLUGIN_STAGE_CLOSE) code = TS_PLUGIN_ERROR_CLOSE;
  else if (status == CMETA_PLUGIN_LOAD_FAILED) code = TS_PLUGIN_ERROR_OPEN;
  return plugin_error_set(error, code, stage, (uint32_t)status, path,
      "Salts Plugin (%s): %s", CMETA_PLUGIN_QUERY_SYMBOL, cmeta_plugin_status_string(status));
}

int ts_plugin_load_ex(const char *path, const char *expected_name,
                      ts_plugin_handle_t **out, ts_plugin_error_t *error) {
  void *bootstrap;
  char *resolved = NULL;
  ts_plugin_handle_t *h;
  const cmeta_plugin_manifest *manifest = NULL;
  const cmeta_plugin_registry_config config = {1u};
  cmeta_plugin_status status;
  int result;

  plugin_error_clear(error);
  if (out) *out = NULL;
  if (!path || !*path || !out)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
        TS_PLUGIN_STAGE_ARGUMENT, 0, path, "plugin path and output handle are required");

  /* Keep the project's restricted dependency search. This temporary OS
   * reference reads no metadata; the registry owns admission and steady life. */
  bootstrap = pl_dlopen(path, &resolved, error);
  if (!bootstrap) return error ? error->code : TS_PLUGIN_ERROR_OPEN;
  h = (ts_plugin_handle_t *)calloc(1, sizeof(*h));
  if (!h) {
    pl_dlclose(bootstrap);
    free(resolved);
    return plugin_error_set(error, TS_PLUGIN_ERROR_OUT_OF_MEMORY,
        TS_PLUGIN_STAGE_OPEN, 0, path, "out of memory creating plugin registry");
  }
  status = cmeta_plugin_registry_init(&h->registry, &config);
  if (status == CMETA_PLUGIN_OK)
    status = cmeta_plugin_registry_load(&h->registry, resolved, &h->ref);
  pl_dlclose(bootstrap);
  free(resolved);
  if (status != CMETA_PLUGIN_OK) {
    result = plugin_salts_error(error, status,
        status == CMETA_PLUGIN_QUERY_MISSING ? TS_PLUGIN_STAGE_SYMBOL : TS_PLUGIN_STAGE_ABI, path);
    ts_plugin_unload(h);
    return result;
  }
  status = cmeta_plugin_registry_start(&h->registry, h->ref);
  if (status == CMETA_PLUGIN_OK)
    status = cmeta_plugin_registry_acquire(&h->registry, h->ref, &h->lease, &manifest);
  if (status != CMETA_PLUGIN_OK) goto rejected;
  h->name = manifest->plugin_id;
  if (expected_name && strcmp(h->name, expected_name) != 0) {
    result = plugin_error_set(error, TS_PLUGIN_ERROR_NAME, TS_PLUGIN_STAGE_ABI,
        0, path, "plugin name mismatch: expected '%s', got '%s'", expected_name, h->name);
    ts_plugin_unload(h);
    return result;
  }
  /* TurboScript owns per-context instances. Managed DSO-global resources are
   * outside this module contract and must not bypass the host close protocol. */
  if (manifest->self || manifest->start || manifest->request_stop ||
      manifest->is_quiescent || manifest->destroy) {
    status = CMETA_PLUGIN_INCOMPATIBLE_CONTRACT;
    goto rejected;
  }
  status = cmeta_plugin_manifest_find_export(manifest, TS_PLUGIN_OPEN_EXPORT, &h->open_export);
  if (status == CMETA_PLUGIN_OK)
    status = cmeta_plugin_manifest_find_export(manifest, TS_PLUGIN_CLOSE_EXPORT, &h->close_export);
  if (status == CMETA_PLUGIN_OK)
    status = cmeta_plugin_export_require_function(h->open_export,
        TS_PLUGIN_CONTRACT_ID, TS_PLUGIN_ABI_VERSION, 0);
  if (status == CMETA_PLUGIN_OK)
    status = cmeta_plugin_export_require_function(h->close_export,
        TS_PLUGIN_CONTRACT_ID, TS_PLUGIN_ABI_VERSION, 0);
  if (status == CMETA_PLUGIN_OK &&
      (!cmeta_function_abi_contract_compatible(FunctionAbi(ts_plugin_open),
          h->open_export->value.function.abi) ||
       !cmeta_function_abi_contract_compatible(FunctionAbi(ts_plugin_close),
          h->close_export->value.function.abi)))
    status = CMETA_PLUGIN_INCOMPATIBLE_CONTRACT;
  if (status != CMETA_PLUGIN_OK) goto rejected;
  *out = h;
  return TS_PLUGIN_ERROR_NONE;

rejected:
  result = plugin_salts_error(error, status, TS_PLUGIN_STAGE_ABI, path);
  ts_plugin_unload(h);
  return result;
}

ts_plugin_handle_t *ts_plugin_load(const char *path) {
  ts_plugin_handle_t *handle = NULL;
  if (ts_plugin_load_ex(path, NULL, &handle, NULL) != TS_PLUGIN_ERROR_NONE) return NULL;
  return handle;
}

int ts_plugin_init_ex(ts_plugin_handle_t *h, void *env, void *scratch,
                      ts_plugin_error_t *error) {
  void *params[] = {&env, &scratch};
  void *instance = NULL;
  cmeta_plugin_lifecycle_info info;
  plugin_error_clear(error);
  if (!h || !h->open_export || !cmeta_plugin_lease_valid(h->lease) || !env || !scratch)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
        TS_PLUGIN_STAGE_ARGUMENT, 0, NULL, "admitted plugin, env and scratch are required");
  if (h->instance ||
      cmeta_plugin_registry_get_lifecycle(&h->registry, h->ref, &info) != CMETA_PLUGIN_OK ||
      info.state != CMETA_PLUGIN_LIFECYCLE_STARTED)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
        TS_PLUGIN_STAGE_INITIALIZE, 0, NULL, "plugin is initialized or stopping");
  if (!h->open_export->value.function.invoke(h->open_export->value.function.context,
          &instance, params, 2u) || !instance)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INITIALIZE,
        TS_PLUGIN_STAGE_INITIALIZE, 0, NULL, "plugin initialization returned no instance");
  h->instance = instance;
  return TS_PLUGIN_ERROR_NONE;
}

int ts_plugin_init(ts_plugin_handle_t *h, void *env, void *scratch) {
  return ts_plugin_init_ex(h, env, scratch, NULL) == TS_PLUGIN_ERROR_NONE ? 0 : -1;
}

int ts_plugin_unload_ex(ts_plugin_handle_t *h, ts_plugin_error_t *error) {
  cmeta_plugin_status status;
  cmeta_plugin_lifecycle_info info;
  bool quiet = false;
  plugin_error_clear(error);
  if (!h) return TS_PLUGIN_ERROR_NONE;
  if (cmeta_plugin_ref_valid(h->ref)) {
    status = cmeta_plugin_registry_get_lifecycle(&h->registry, h->ref, &info);
    if (status != CMETA_PLUGIN_OK) goto failed;
    if (info.state != CMETA_PLUGIN_LIFECYCLE_LOADED) {
      status = cmeta_plugin_registry_request_stop(&h->registry, h->ref);
      if (status != CMETA_PLUGIN_OK && status != CMETA_PLUGIN_ALREADY) goto failed;
      if (h->instance) {
        int32_t close_status = -1;
        void *params[] = {&h->instance};
        if (!h->close_export->value.function.invoke(h->close_export->value.function.context,
                &close_status, params, 1u) || close_status != 0)
          return plugin_error_set(error, TS_PLUGIN_ERROR_CLOSE, TS_PLUGIN_STAGE_CLOSE,
              (uint32_t)close_status, NULL, "plugin close failed; instance and lease retained");
        h->instance = NULL;
      }
      if (cmeta_plugin_lease_valid(h->lease)) {
        status = cmeta_plugin_registry_release(&h->registry, &h->lease);
        if (status != CMETA_PLUGIN_OK) goto failed;
        h->name = NULL;
        h->open_export = h->close_export = NULL;
      }
      status = cmeta_plugin_registry_poll_quiescent(&h->registry, h->ref, &quiet);
      if (status != CMETA_PLUGIN_OK) goto failed;
      if (!quiet) { status = CMETA_PLUGIN_BUSY; goto failed; }
    }
    status = cmeta_plugin_registry_unload(&h->registry, h->ref);
    if (status != CMETA_PLUGIN_OK) goto failed;
    memset(&h->ref, 0, sizeof(h->ref));
  }
  if (h->registry.impl) {
    status = cmeta_plugin_registry_destroy(&h->registry);
    if (status != CMETA_PLUGIN_OK) goto failed;
  }
  free(h);
  return TS_PLUGIN_ERROR_NONE;
failed:
  return plugin_salts_error(error, status, TS_PLUGIN_STAGE_CLOSE, NULL);
}

void ts_plugin_unload(ts_plugin_handle_t *h) {
  /* Void context destruction cannot transfer a retry obligation. */
  if (ts_plugin_unload_ex(h, NULL) != TS_PLUGIN_ERROR_NONE) abort();
}
