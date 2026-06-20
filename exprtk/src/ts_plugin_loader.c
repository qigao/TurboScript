/**
 * @file ts_plugin_loader.c
 * @brief Cross-platform plugin loader implementation.
 */
#include "ts_plugin_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Platform dlopen shim ─────────────────────────────────────────── */
#ifdef _WIN32
  #include <windows.h>

static int has_path_separator_win(const char *path) {
  return path && (strchr(path, '\\') || strchr(path, '/') || strchr(path, ':'));
}

static int get_exe_dir_win(char *buf, size_t buf_len) {
  DWORD len;
  char *slash;

  if (!buf || buf_len == 0)
    return -1;

  len = GetModuleFileNameA(NULL, buf, (DWORD)buf_len);
  if (len == 0 || len >= buf_len)
    return -1;

  /* 查找最后一个路径分隔符 */
  slash = strrchr(buf, '\\');
  if (!slash)
    slash = strrchr(buf, '/');
  
  if (!slash)
    return -1;
  
  *slash = '\0';
  return 0;
}

static void *pl_dlopen(const char *path) { 
  void *dl;
  char exe_dir[MAX_PATH];
  char candidate[MAX_PATH];
  int written;

  /* 先尝试直接加载 */
  dl = (void *)LoadLibraryA(path);
  if (dl)
    return dl;

  /* 如果路径包含分隔符，不再尝试其他位置 */
  if (has_path_separator_win(path))
    return NULL;

  /* 尝试从可执行文件目录加载 */
  if (get_exe_dir_win(exe_dir, sizeof(exe_dir)) != 0)
    return NULL;

  written = snprintf(candidate, sizeof(candidate), "%s\\%s", exe_dir, path);
  if (written > 0 && written < (int)sizeof(candidate)) {
    dl = (void *)LoadLibraryA(candidate);
    if (dl)
      return dl;
  }

  return NULL;
}

static void *pl_dlsym(void *h, const char *name) {
  return (void *)GetProcAddress((HMODULE)h, name);
}
static void pl_dlclose(void *h) { FreeLibrary((HMODULE)h); }
#else
  #include <dlfcn.h>
  #include <limits.h>
  #include <unistd.h>

static int has_path_separator(const char *path) {
  return path && (strchr(path, '/') || strchr(path, '\\'));
}

static int get_exe_dir(char *buf, size_t buf_len) {
  ssize_t len;
  char *slash;

  if (!buf || buf_len == 0)
    return -1;

  len = readlink("/proc/self/exe", buf, buf_len - 1);
  if (len <= 0 || (size_t)len >= buf_len)
    return -1;
  buf[len] = '\0';

  slash = strrchr(buf, '/');
  if (!slash)
    return -1;
  *slash = '\0';
  return 0;
}

static void *pl_dlopen(const char *path) {
  void *dl;
  int written;
  char exe_dir[PATH_MAX];
  char candidate[PATH_MAX];

  /* 先尝试直接加载 */
  dl = dlopen(path, RTLD_LAZY);
  if (dl || has_path_separator(path))
    return dl;

  /* 尝试从可执行文件目录加载 */
  if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
    return NULL;

  written = snprintf(candidate, sizeof(candidate), "%s/%s", exe_dir, path);
  if (written > 0 && written < (int)sizeof(candidate)) {
    dl = dlopen(candidate, RTLD_LAZY);
    if (dl)
      return dl;
  }

  return NULL;
}

static void *pl_dlsym(void *h, const char *name) { return dlsym(h, name); }
static void pl_dlclose(void *h) { dlclose(h); }
#endif

ts_plugin_handle_t *ts_plugin_load(const char *path) {
  void *dl;
  ts_api_create_fn create_fn;
  const ts_plugin_t *plugin;
  ts_plugin_handle_t *h;

  if (!path)
    return NULL;

  dl = pl_dlopen(path);
  if (!dl)
    return NULL;

  create_fn = (ts_api_create_fn)pl_dlsym(dl, "ts_api_create");
  if (!create_fn) {
    pl_dlclose(dl);
    return NULL;
  }

  plugin = create_fn();
  if (!plugin || !plugin->load) {
    pl_dlclose(dl);
    return NULL;
  }

  h = (ts_plugin_handle_t *)calloc(1, sizeof(*h));
  if (!h) {
    pl_dlclose(dl);
    return NULL;
  }

  h->dl_handle = dl;
  h->plugin = plugin;
  h->instance = NULL;
  return h;
}

int ts_plugin_init(ts_plugin_handle_t *h, void *env, void *scratch) {
  if (!h || !h->plugin || !h->plugin->load)
    return -1;
  h->instance = h->plugin->load(env, scratch);
  if (!h->instance)
    return -1;
  return 0;
}

void ts_plugin_unload(ts_plugin_handle_t *h) {
  if (!h)
    return;
  if (h->plugin && h->plugin->unload)
    h->plugin->unload(h->instance);
  if (h->dl_handle)
    pl_dlclose(h->dl_handle);
  free(h);
}
