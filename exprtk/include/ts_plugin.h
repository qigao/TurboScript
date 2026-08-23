/**
 * @file ts_plugin.h
 * @brief TurboScript plugin ABI — zero dependencies.
 *
 * Every plugin DLL exports exactly one function: ts_api_create().
 * It returns a pointer to a static ts_plugin_t descriptor.
 *
 * Two convenience macros eliminate boilerplate for common patterns:
 *   TS_PLUGIN_MODULE   — stateless plugin that registers an exprtk_module_t
 *   TS_PLUGIN_STATEFUL — stateful plugin with create/load/destroy lifecycle
 */
#ifndef TS_PLUGIN_H
#define TS_PLUGIN_H

#include "exprtk_export.h"

#include "platform.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — avoids pulling in exprtk.h / turbo_buffer.h */
typedef struct exprtk_env_s exprtk_env_t;
typedef struct mem_pool_s mem_pool_t;
typedef struct exprtk_module_s exprtk_module_t;
EXPRTK_C_API void exprtk_env_add_module(exprtk_env_t *env, const exprtk_module_t *mod);

typedef struct ts_plugin_s {
  const char *name; /* "ta", "csv", "vec", ... */
  uint32_t version; /* ABI version (1) */

  /**
   * Called by turbo_script when import("<name>") is executed.
   * Plugin registers its functions into env.
   * @param env     exprtk_env_t*  — register functions here
   * @param scratch mem_pool_t* — temp allocator
   * @return opaque plugin instance (passed to unload), or NULL
   */
  void *(*load)(void *env, void *scratch);

  /**
   * Called when turbo_script context is freed.
   * @param instance — the value returned by load()
   */
  void (*unload)(void *instance);
} ts_plugin_t;

/* Every plugin DLL exports exactly this function */
typedef const ts_plugin_t *(*ts_api_create_fn)(void);

/* Plugin entry points are always exported by the plugin binary itself. */
#if defined(_WIN32) || defined(__CYGWIN__)
#define TS_PLUGIN_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#define TS_PLUGIN_API __attribute__((visibility("default")))
#else
#define TS_PLUGIN_API
#endif

#ifdef __cplusplus
#define TS_PLUGIN_C_API extern "C" TS_PLUGIN_API
#else
#define TS_PLUGIN_C_API TS_PLUGIN_API
#endif

/* ── Stateless plugin: registers one exprtk_module_t, no instance ── */
#define TS_PLUGIN_MODULE(plugin_name, module_fn)                                                   \
  static void *ts__##plugin_name##_load(void *env, void *scratch) {                                \
    (void)scratch;                                                                                 \
    const exprtk_module_t *mod = module_fn();                                                      \
    if (!env || !mod)                                                                              \
      return NULL;                                                                                 \
    exprtk_env_add_module((exprtk_env_t *)env, mod);                                               \
    return env;                                                                                    \
  }                                                                                                \
  static void ts__##plugin_name##_unload(void *inst) { (void)inst; }                               \
  static const ts_plugin_t g_##plugin_name = {                                                     \
      .name = #plugin_name,                                                                        \
      .version = 1,                                                                                \
      .load = ts__##plugin_name##_load,                                                            \
      .unload = ts__##plugin_name##_unload,                                                        \
  };                                                                                               \
  TS_PLUGIN_C_API const ts_plugin_t *ts_api_create(void) { return &g_##plugin_name; }

/* ── Stateful plugin: create ctx → register funcs → destroy ctx ──── */
#define TS_PLUGIN_STATEFUL(plugin_name, create_fn, loader_fn, destroy_fn)                          \
  static void *ts__##plugin_name##_load(void *env, void *scratch) {                                \
    void *ctx = (void *)create_fn();                                                               \
    if (!ctx)                                                                                      \
      return NULL;                                                                                 \
    loader_fn(ctx, env, scratch);                                                                  \
    return ctx;                                                                                    \
  }                                                                                                \
  static void ts__##plugin_name##_unload(void *inst) {                                             \
    if (inst)                                                                                      \
      destroy_fn(inst);                                                                            \
  }                                                                                                \
  static const ts_plugin_t g_##plugin_name = {                                                     \
      .name = #plugin_name,                                                                        \
      .version = 1,                                                                                \
      .load = ts__##plugin_name##_load,                                                            \
      .unload = ts__##plugin_name##_unload,                                                        \
  };                                                                                               \
  TS_PLUGIN_C_API const ts_plugin_t *ts_api_create(void) { return &g_##plugin_name; }

#ifdef __cplusplus
}
#endif

#endif /* TS_PLUGIN_H */
