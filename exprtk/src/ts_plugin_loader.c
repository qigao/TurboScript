/**
 * @file ts_plugin_loader.c
 * @brief Cross-platform plugin loader implementation.
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

static void *open_exact_win(const wchar_t *path, const wchar_t *dependency_dir,
                            DWORD *native_error) {
  DWORD length;
  wchar_t *absolute_path;
  DLL_DIRECTORY_COOKIE dependency_cookie;
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
  length = GetFullPathNameW(path, 0, NULL, NULL);
  if (length == 0) {
    if (native_error) *native_error = GetLastError();
    RemoveDllDirectory(dependency_cookie);
    return NULL;
  }
  absolute_path = (wchar_t *)malloc((size_t)length * sizeof(*absolute_path));
  if (!absolute_path) {
    if (native_error) *native_error = ERROR_NOT_ENOUGH_MEMORY;
    RemoveDllDirectory(dependency_cookie);
    return NULL;
  }
  if (GetFullPathNameW(path, length, absolute_path, NULL) == 0) {
    if (native_error) *native_error = GetLastError();
    free(absolute_path);
    RemoveDllDirectory(dependency_cookie);
    return NULL;
  }
  module = LoadLibraryExW(absolute_path, NULL, flags);
  if (!module && native_error) *native_error = GetLastError();
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

static void *pl_dlopen(const char *path, ts_plugin_error_t *error) {
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

static void *pl_dlsym(void *h, const char *name) {
  return (void *)GetProcAddress((HMODULE)h, name);
}
static int pl_symbol_error(ts_plugin_error_t *error, const char *path, const char *name) {
  DWORD native_error = GetLastError();
  return plugin_error_set(error, TS_PLUGIN_ERROR_SYMBOL, TS_PLUGIN_STAGE_SYMBOL,
                          native_error, path, "required symbol '%s' was not found", name);
}
static void pl_dlclose(void *h) { FreeLibrary((HMODULE)h); }
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

static void *open_exact_posix(const char *path) {
  (void)dlerror();
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void *pl_dlopen(const char *path, ts_plugin_error_t *error) {
  void *module = NULL;
  const char *detail = NULL;
  char *exe_dir;
  char *plugins_dir;
  char *candidate;

  if (has_path_separator(path)) {
    module = open_exact_posix(path);
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
  if (candidate) module = open_exact_posix(candidate);
  free(candidate);
  free(plugins_dir);
  if (!module) {
    candidate = join_path_posix(exe_dir, path);
    if (candidate) module = open_exact_posix(candidate);
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

static void *pl_dlsym(void *h, const char *name) {
  (void)dlerror();
  return dlsym(h, name);
}
static int pl_symbol_error(ts_plugin_error_t *error, const char *path, const char *name) {
  const char *detail = dlerror();
  return plugin_error_set(error, TS_PLUGIN_ERROR_SYMBOL, TS_PLUGIN_STAGE_SYMBOL, 0,
                          path, "required symbol '%s' was not found: %s", name,
                          detail ? detail : "dlsym failed");
}
static void pl_dlclose(void *h) { dlclose(h); }
#endif

int ts_plugin_load_ex(const char *path, const char *expected_name,
                      ts_plugin_handle_t **out, ts_plugin_error_t *error) {
  void *dl;
  ts_api_create_fn create_fn;
  const ts_plugin_t *plugin;
  ts_plugin_handle_t *h;

  plugin_error_clear(error);
  if (out) *out = NULL;
  if (!path || !*path || !out)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_ARGUMENT, 0, path,
                            "plugin path and output handle are required");

  dl = pl_dlopen(path, error);
  if (!dl) return error ? error->code : TS_PLUGIN_ERROR_OPEN;

  create_fn = (ts_api_create_fn)pl_dlsym(dl, "ts_api_create");
  if (!create_fn) {
    int result = pl_symbol_error(error, path, "ts_api_create");
    pl_dlclose(dl);
    return result;
  }

  plugin = create_fn();
  if (!plugin || !plugin->name || !*plugin->name || !plugin->load) {
    pl_dlclose(dl);
    return plugin_error_set(error, TS_PLUGIN_ERROR_DESCRIPTOR,
                            TS_PLUGIN_STAGE_ABI, 0, path,
                            "plugin descriptor is incomplete");
  }
  if (plugin->version != TS_PLUGIN_ABI_VERSION) {
    uint32_t actual_version = plugin->version;
    pl_dlclose(dl);
    return plugin_error_set(error, TS_PLUGIN_ERROR_ABI, TS_PLUGIN_STAGE_ABI,
                            0, path, "plugin ABI mismatch: expected %u, got %u",
                            (unsigned)TS_PLUGIN_ABI_VERSION, (unsigned)actual_version);
  }
  if (expected_name && strcmp(plugin->name, expected_name) != 0) {
    char actual_name[128];
    snprintf(actual_name, sizeof(actual_name), "%s", plugin->name);
    pl_dlclose(dl);
    return plugin_error_set(error, TS_PLUGIN_ERROR_NAME, TS_PLUGIN_STAGE_ABI,
                            0, path, "plugin name mismatch: expected '%s', got '%s'",
                            expected_name, actual_name);
  }

  h = (ts_plugin_handle_t *)calloc(1, sizeof(*h));
  if (!h) {
    pl_dlclose(dl);
    return plugin_error_set(error, TS_PLUGIN_ERROR_OUT_OF_MEMORY,
                            TS_PLUGIN_STAGE_OPEN, 0, path,
                            "out of memory while creating plugin handle");
  }

  h->dl_handle = dl;
  h->plugin = plugin;
  h->instance = NULL;
  *out = h;
  plugin_error_clear(error);
  return TS_PLUGIN_ERROR_NONE;
}

ts_plugin_handle_t *ts_plugin_load(const char *path) {
  ts_plugin_handle_t *handle = NULL;
  if (ts_plugin_load_ex(path, NULL, &handle, NULL) != TS_PLUGIN_ERROR_NONE)
    return NULL;
  return handle;
}

int ts_plugin_init_ex(ts_plugin_handle_t *h, void *env, void *scratch,
                      ts_plugin_error_t *error) {
  plugin_error_clear(error);
  if (!h || !h->plugin || !h->plugin->load)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_ARGUMENT, 0, NULL,
                            "validated plugin handle is required");
  if (h->instance)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TS_PLUGIN_STAGE_INITIALIZE, 0, NULL,
                            "plugin is already initialized");
  h->instance = h->plugin->load(env, scratch);
  if (!h->instance)
    return plugin_error_set(error, TS_PLUGIN_ERROR_INITIALIZE,
                            TS_PLUGIN_STAGE_INITIALIZE, 0, NULL,
                            "plugin initialization returned no instance");
  return TS_PLUGIN_ERROR_NONE;
}

int ts_plugin_init(ts_plugin_handle_t *h, void *env, void *scratch) {
  return ts_plugin_init_ex(h, env, scratch, NULL) == TS_PLUGIN_ERROR_NONE ? 0 : -1;
}

void ts_plugin_unload(ts_plugin_handle_t *h) {
  if (!h)
    return;
  if (h->instance && h->plugin && h->plugin->unload)
    h->plugin->unload(h->instance);
  if (h->dl_handle)
    pl_dlclose(h->dl_handle);
  free(h);
}
