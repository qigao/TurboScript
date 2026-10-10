#include "ts_plugin.h"

#ifndef FIXTURE_PLUGIN_NAME
#define FIXTURE_PLUGIN_NAME "loader_fixture"
#endif

#if defined(FIXTURE_INIT_FAILURE)
static int *failed_init_close_count;
#endif

void *ts_plugin_open(void *env, void *scratch) {
  (void)scratch;
#if defined(FIXTURE_INIT_FAILURE)
  failed_init_close_count = (int *)scratch;
  (void)env;
  return NULL;
#else
  return env;
#endif
}

int32_t ts_plugin_close(void *instance) {
#if defined(FIXTURE_INIT_FAILURE)
  (void)instance;
  ++*failed_init_close_count;
  return 0;
#elif defined(FIXTURE_CLOSE_FAILURE)
  int *attempts = (int *)instance;
  return ++*attempts == 1 ? -7 : 0;
#else
  (void)instance;
  return 0;
#endif
}

#if defined(FIXTURE_ABI_VERSION)
CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL
cmeta_plugin_query(uint32_t host_abi) {
  (void)host_abi;
  return NULL; /* A provider compiled for another epoch rejects bootstrap. */
}
#elif defined(FIXTURE_BAD_CONTRACT)
#define FIXTURE_EXPORTS(X) \
  X(function, ts_plugin_open, "open", TS_PLUGIN_CONTRACT_ID, 999, 0) \
  X(function, ts_plugin_close, "close", TS_PLUGIN_CONTRACT_ID, 999, 0)
CMETA_PLUGIN_DECLARE(fixture, FIXTURE_PLUGIN_NAME, (2, 0, 0),
    FIXTURE_EXPORTS, CMETA_PLUGIN_PASSIVE());
#elif defined(FIXTURE_BAD_SIGNATURE)
int fixture_wrong_open(void) { return 0; }
Function0InvokeDecl(fallible, int, fixture_wrong_open);
#define FIXTURE_EXPORTS(X) \
  X(function, fixture_wrong_open, "open", TS_PLUGIN_CONTRACT_ID, TS_PLUGIN_ABI_VERSION, 0) \
  X(function, ts_plugin_close, "close", TS_PLUGIN_CONTRACT_ID, TS_PLUGIN_ABI_VERSION, 0)
CMETA_PLUGIN_DECLARE(fixture, FIXTURE_PLUGIN_NAME, (2, 0, 0),
    FIXTURE_EXPORTS, CMETA_PLUGIN_PASSIVE());
#else
TS_PLUGIN_PUBLISH(FIXTURE_PLUGIN_NAME)
#endif
