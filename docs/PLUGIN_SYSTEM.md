# TurboScript Native Plugin System

TurboScript uses the **current Salts Plugin ABI** for native plugin discovery,
loading, lifecycle, leases, and capability publication.

TurboScript does not define a second generic native-plugin ABI.

## Architecture

```text
import("fin")
   |
   v
TurboScript language/import layer
   |
   v
Salts::Plugin registry
  load -> start -> acquire lease
   |
   v
salts_plugin_manifest
   |
   +--> FUNCTION export
   |      CMeta FunctionDesc
   |      CMeta FunctionAbi
   |      exact invoke/context
   |
   +--> INTERFACE export
          CMeta InterfaceDesc
          { self, vtable }
   |
   v
TurboScript binding cache
   |
   +--> interpreter / JIT calls
   +--> CFlow admission for graphable Functions
```

### Ownership

- **Salts::Plugin** owns DSO loading, the single Plugin ABI, lifecycle, and
  leases.
- **CMeta** owns native function/interface type, ABI, effects, properties,
  direction, and ownership semantics.
- **TurboScript** owns `import()`, script namespaces, script-value conversion,
  diagnostics, interpreter/JIT execution, and Host Module ABI.
- **CFlow** may consume a pre-bound canonical Function when its reflected
  effects and type contract allow graph admission.

The Host Module ABI used to execute compiled TurboScript programs is separate
from the Salts Plugin ABI used to publish native capabilities.

## Loading and discovery

For a logical import such as:

```javascript
import("fin");
```

TurboScript resolves the platform plugin filename and loads it through
`salts_plugin_registry_*`. The DSO must publish the current
`salts_plugin_query` entry and a valid current-ABI manifest.

The loader searches package/executable plugin locations; it does not search the
process current working directory.

Typical names:

```text
Windows  fin.dll
Linux    fin.so
macOS    fin.dylib
```

Some repository plugins use collision-safe physical filenames while keeping the
same logical plugin ID.

## Canonical Function exports

Use a Function export for ordinary stateless native operations.

The semantic source of truth is:

```text
FunctionDesc + FunctionAbi + exact invoke adapter
```

The TurboScript dispatch row is only a cached language binding.

Example shape:

```c
#include <salts/plugin.h>
#include <salts/thread.h>

FunctionDeclResult(value, double, CMETA_RESULT_VALUE, my_double,
    (double, value, CMETA_PARAM_IN));

double my_double(double value) {
    return value * 2.0;
}

static bool SALTS_PLUGIN_CALL my_double_invoke(
    void *context,
    void *return_storage,
    void *const *params,
    size_t param_count) {
    if (context != NULL || return_storage == NULL ||
        params == NULL || param_count != 1u || params[0] == NULL)
        return false;

    *(double *)return_storage =
        my_double(*(const double *)params[0]);
    return true;
}

static salts_plugin_export exports[1];
static salts_once_t exports_once = SALTS_ONCE_INIT;

static void init_exports(void) {
    exports[0] = (salts_plugin_export){
        .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
        .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
        .contract_version = 1u,
        .capabilities = 0u,
        .export_id = "math.double",
        .contract_id = "example.math",
        .value.function = {
            .desc = FunctionMeta(my_double),
            .abi = FunctionAbi(my_double),
            .context = NULL,
            .invoke = my_double_invoke,
        },
    };
}

static const salts_plugin_manifest manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "my_math",
    .version = {1u, 0u, 0u},
    .exports = exports,
    .export_count = 1u,
};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
    if (host_abi != SALTS_PLUGIN_ABI_VERSION) return NULL;
    salts_once(&exports_once, init_exports);
    return &manifest;
}
```

After `import("my_math")`, the script-visible name comes from
`export_id`:

```javascript
value = math.double(3.5);
```

### Current scalar binding boundary

TurboScript currently binds the finite CMeta scalar universe directly:

- `bool`
- `int`
- `long`
- `float`
- `double`
- zero through 16 `IN` scalar parameters
- scalar or `void` return

Pointer/object/aggregate/opaque and OUT/INOUT signatures require an explicit
future language binding rather than guessed dynamic invocation.

Integer-valued canonical bindings preserve exact ExprTk value types instead of
passing through legacy integer-to-double normalization.

## CFlow admission

Plugin Functions use the same CMeta/CFlow path as other graphable callables.

A PURE compatible reflected Function may be admitted to a CFlow node. Its
`FunctionDesc` supplies effects/properties and its `FunctionAbi` supplies the
native ABI contract.

```text
Plugin Function
  -> retained lease
  -> FunctionDesc / FunctionAbi
  -> pre-bound exact adapter
  -> CFlow projection
  -> Graph / Plan
```

Functions marked STATEFUL, IO, ASYNC, MAY_FAIL where incompatible, or UNKNOWN
remain conservative graph barriers. TurboScript does not maintain a parallel
plugin purity registry.

The plugin lease must remain live for every graph/plan/run that can reach
plugin-owned descriptors, code, traits, context, or interface values.

## Canonical Interface exports

Use an Interface export for long-lived stateful or polymorphic providers.

Examples include:

- database/resource providers,
- network/session providers,
- device runtimes,
- other capabilities naturally represented as `{self, vtable}`.

The Interface descriptor is the native semantic contract. Script adapters
should convert TurboScript values at the language boundary instead of exposing
ExprTk types in the provider Interface.

## Migration helpers in this repository

`TS_PLUGIN_MODULE`, `TS_PLUGIN_MODULE_WITH_UNLOAD`, and
`TS_PLUGIN_STATEFUL` publish capabilities through the current Salts Plugin
manifest and are retained only to migrate existing modules incrementally.

They are **not** a second loader or ABI.

For new native capability design:

1. prefer canonical FUNCTION exports for ordinary operations;
2. prefer canonical INTERFACE exports for stateful providers;
3. use generated DataBind/CMeta contracts where a logical Service/Component
   contract already exists;
4. keep script namespace and conversion logic in TurboScript.

## Hot-path rule

Plugin discovery and reflection are control-plane work.

After import/binding, normal calls use an already-bound exact adapter or
provider vtable.

There must be no per-call:

- DSO symbol lookup,
- plugin registry lookup,
- manifest lookup,
- ABI negotiation,
- reflection catalog lookup.

## Lifecycle

TurboScript retains the Salts Plugin lease while plugin-owned callbacks,
descriptors, interfaces, or plan callables remain reachable.

Context teardown removes script-visible references before releasing the lease
and unloading the DSO.

## Building

Plugins should link against the installed/current Salts Plugin ABI and CMeta
headers rather than copying ABI structs into project-local headers.

In this repository, use the normal CMake dependency graph and
`Salts::Plugin` / `Salts::PluginABI` targets as appropriate.

## Troubleshooting

### Plugin fails at open stage

Check that the platform DSO exists in the configured plugin/package location
and that all of its dynamic dependencies are available.

### Plugin fails at symbol/ABI stage

Ensure the DSO publishes `salts_plugin_query` and was rebuilt against the
current Salts Plugin ABI.

There is no fallback to an older TurboScript-private plugin ABI.

### Function export fails initialization

Check:

- valid FunctionDesc and FunctionAbi,
- exact adapter is non-null,
- parameter directions/carriers are supported by the current TurboScript
  binding,
- `export_id` does not conflict with an existing environment function.

## See also

- `exprtk/include/ts_plugin.h` — TurboScript migration adapters over Salts
  Plugin Interface publication.
- `turbo_script/include/ts_plugin_loader.h` — TurboScript binding adapter.
- issue #22 — Plugin ABI/CMeta convergence tracking.
- issue #84 — TurboDB::Orm database integration with explicit user driver configuration.
