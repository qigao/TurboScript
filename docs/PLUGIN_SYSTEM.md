# TurboScript Plugin System

TurboScript's native modules use Salts Plugin. The built-in dynamic modules are
net, ta, fin, ts, sqlite, mapper, img, crypto, hash, fuzzy and os. Script imports
and function names are unchanged. The net module supplies both `http.*` and `ws.*`
through `CHttp::Client`; Salts Plugin manages the DLL lifetime, not network progress.

## Versions and rebuilding

All native plugins must be rebuilt with the current `ts_plugin.h` and matching
TurboScript/Salts SDKs. The old `ts_api_create` descriptor is no longer admitted.

| Boundary | Current version |
| --- | --- |
| TurboScript module contract | `TS_PLUGIN_ABI_VERSION == 2` |
| Salts Plugin publication ABI in the selected SDK | `CMETA_PLUGIN_ABI_VERSION == 5` |
| Separate host module API | `TURBO_SCRIPT_HOST_ABI_VERSION == 1` |

The export contract ID is `qigao.turboscript.module`. Package versions are not ABI
epochs. Rebuild and deploy host and plugin artifacts together; rollback also
restores the complete matching set.

## Publication

Include `ts_plugin.h` and link `Salts::PluginABI`. A DSO publishes one immutable,
passive manifest with `open` and `close` Function exports. CMeta generates exact
adapters and metadata from the same declarations. The host checks contract ID,
version and native Function ABI compatibility before invoking either export.

A stateless module uses the existing convenience macro:

```c
#include "exprtk_module.h"
#include "ts_plugin.h"

static exprtk_value_t answer(size_t argc, exprtk_value_t *args,
                             exprtk_env_t *env, mem_pool_t *scratch) {
    (void)argc; (void)args; (void)env; (void)scratch;
    return exprtk_val_int(42);
}
static const exprtk_func_entry_t entries[] = {{"answer", answer}};
static const exprtk_module_t module = {"example", entries, 1};
static const exprtk_module_t *example_module(void) { return &module; }
TS_PLUGIN_MODULE(example, example_module)
```

After building `example.dll` (or the platform shared-library equivalent), scripts
can call `import("example"); example.answer();`.

For stateful modules, use
`TS_PLUGIN_STATEFUL(name, create_fn, loader_fn, destroy_fn)` when destruction
cannot fail. The callbacks have these contracts:

- `create_fn()` returns a new context or NULL, with no outstanding obligation on failure.
- `loader_fn(context, env, scratch)` registers functions and borrows the host owners.
- `destroy_fn(context)` finishes all cleanup and cannot leave work in flight.

Use `TS_PLUGIN_STATEFUL_CHECKED(name, create_fn, loader_fn, close_fn)` for fallible
cleanup. `close_fn(void *instance)` returns zero only after consuming the instance.
Any nonzero result preserves the instance and all unfinished resources for retry.
The network plugin uses this form.

Custom publication defines `ts_plugin_open(void *env, void *scratch)` and
`ts_plugin_close(void *instance)`, then calls `TS_PLUGIN_PUBLISH("name")`.
Do not independently reproduce their signatures or manifest rows.

An open result is an owned logical instance handle, not permission for the host
to call free. Stateless modules can alias env as that handle with a no-op close.
NULL means initialization failed. A provider must roll back partially created
resources and registrations before returning NULL. The convenience stateful
loader callback is void; providers needing fallible registration must implement
custom open/close with explicit rollback.

## Loading and ownership

The host links `Salts::Plugin`. Each loaded handle owns a capacity-one registry
and a lease covering metadata, registered function pointers, calls and cleanup.
A TurboScript context remains bounded to 16 dynamic plugins. Separate contexts
own separate instances even when the OS shares the same DLL mapping.

Discovery preserves the project's established explicit paths and executable
locations. On Windows, configured package and runtime dependency directories are
used without searching the working directory or PATH. The host first opens the
exact candidate with that policy, then asks Salts to admit the resolved path
while the temporary OS reference remains live. It releases that reference once
the registry has taken its own reference. There is no legacy ABI fallback.
Deployment files must remain unchanged throughout loading.

Shutdown order:

1. Stop context tasks/timers and all calls; release JIT and env values that borrow plugin code.
2. Close registry lease admission and invoke the instance close export under the retained lease.
3. If close fails, retain the instance and lease. Retry `ts_plugin_unload_ex()`.
4. After successful close, release the lease, establish quiescence and unload through the registry.

`ts_plugin_unload_ex(handle, error)` consumes the handle on success; a failure
leaves it owned by the caller. No new initialization is allowed after stop begins.
NULL succeeds. The older void `ts_plugin_unload()` and context destruction cannot
return a retry obligation and therefore abort on cleanup failure. Applications
requiring retry use the explicit loader close API.

HTTP/WebSocket destroy errors do not permit freeing the client wrapper. The net
context retains failed clients, clears only successfully destroyed owners, and
retries remaining work. This does not add hot reload, process isolation, a new
poller, or a background I/O thread.

## Build and validation

The repository's module targets inherit `Salts::PluginABI` through ExprTk.
External modules link `TurboScript::ExprTk` and `Salts::PluginABI`, use matching
installed SDK roots and the project's prescribed CMake user presets.

Formal tests:

- `test_ts_plugin_loader`: discovery, ABI/name/contract/signature rejection,
  old-entry rejection, initialization failure, lease retention and close retry.
- `test_ts_builtin_plugins`: all 11 real plugin DLLs in two contexts, repeated
  import and continued use after one context is destroyed.
- `test_net_ctx`: network context lifecycle and helpers.
- Existing script/module tests: import behavior, authorization and function behavior.

Use CTest through `ci-win-release-user` or the appropriate platform user preset.
The external HTTP test in `test_turbo_script_io` is opt-in with `TS_NET_TESTS=1`;
an offline passing suite does not establish public-network HTTPS coverage.

See the [architecture decision](superpowers/specs/2026-08-25-turboscript-host-module-abi-design.md#46-全部内置插件迁移至-salts-plugin)
and the authoritative [publication header](../exprtk/include/ts_plugin.h).
