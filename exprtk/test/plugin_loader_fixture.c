#include "ts_plugin.h"

#ifndef FIXTURE_PLUGIN_NAME
#define FIXTURE_PLUGIN_NAME "loader_fixture"
#endif

#ifndef FIXTURE_ABI_VERSION
#define FIXTURE_ABI_VERSION 1
#endif

#if defined(FIXTURE_INIT_FAILURE)
static int *loader_fixture_failed_init_unload_count;
#endif

static void *loader_fixture_load(void *env, void *scratch) {
#if defined(FIXTURE_INIT_FAILURE)
  loader_fixture_failed_init_unload_count = (int *)scratch;
  (void)env;
  return NULL;
#else
  (void)scratch;
  return env;
#endif
}

static void loader_fixture_unload(void *instance) {
#if defined(FIXTURE_INIT_FAILURE)
  if (!instance && loader_fixture_failed_init_unload_count)
    ++*loader_fixture_failed_init_unload_count;
#else
  (void)instance;
#endif
}

static const ts_plugin_t loader_fixture = {
    .name = FIXTURE_PLUGIN_NAME,
    .version = FIXTURE_ABI_VERSION,
    .load = loader_fixture_load,
    .unload = loader_fixture_unload,
};

TS_PLUGIN_C_API const ts_plugin_t *ts_api_create(void) {
  return &loader_fixture;
}
