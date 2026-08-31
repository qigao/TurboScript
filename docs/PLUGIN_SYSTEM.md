# TurboScript Plugin System

> **🔌 Dynamic module loading and third-party DLL integration**

---

## 📋 Overview

TurboScript uses a **two-tier module system**:

1. **Built-in modules** - Statically embedded in `turbo_script.dll`, registered at startup
2. **Plugin modules** - Dynamically loaded DLLs, loaded on-demand via `import()`

---

## 🎯 Architecture

### Two-Tier Registration System

```
┌─────────────────────────────────────────────────────────────┐
│                    TurboScript Runtime                       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────────┐      ┌──────────────────────┐   │
│  │   Built-in Modules   │      │   Plugin Modules     │   │
│  │  (Compile-time)      │      │  (Runtime-loaded)    │   │
│  ├──────────────────────┤      ├──────────────────────┤   │
│  │ • math               │      │ • ta.dll             │   │
│  │ • string             │      │ • fin.dll            │   │
│  │ • stats              │      │ • net.dll            │   │
│  │ • io                 │      │ • sqlite.dll         │   │
│  │ • core               │      │ • custom.dll         │   │
│  └──────────────────────┘      └──────────────────────┘   │
│           ↓                              ↓                  │
│  Global Registry                  Per-Context Env          │
│  (exprtk_registry)               (exprtk_env_t)            │
└─────────────────────────────────────────────────────────────┘
```

---

## 🔧 Built-in Modules

### Registration Flow

```c
// turbo_script_registry.c
void turbo_script_register_modules(void) {
    // Register built-in modules to global registry
    exprtk_registry_add_module(exprtk_module_math());
    exprtk_registry_add_module(exprtk_module_string());
    exprtk_registry_add_module(exprtk_module_stats());
    exprtk_registry_add_module(exprtk_module_io());
    exprtk_registry_add_module(exprtk_module_core());

    exprtk_registry_init(); // Sort the registry
}
```

**Characteristics:**
- ✅ Statically embedded in `turbo_script.dll`
- ✅ Always available (no `import()` needed)
- ✅ Registered to **global registry**
- ✅ Zero runtime overhead

**Available Functions:**
```javascript
// No import needed - always available
let x = sqrt(16);        // math module
let s = upper("hello");  // string module
let m = mean([1,2,3]);   // stats module
print("Hello");          // io module
let t = typeof(x);       // core module
```

---

## 🔌 Plugin Modules

### Plugin Loading Flow

```
User Script: import("ta")
    ↓
turbo_script_load_plugin("ta")
    ↓
Search for "ta.dll" in plugin paths
    ↓
ts_plugin_load("ta.dll")
    ↓
dlopen/LoadLibrary (OS-level DLL load)
    ↓
Resolve symbol: ts_api_create()
    ↓
Call ts_api_create() → returns ts_plugin_t*
    ↓
Call plugin->load(env, scratch)
    ↓
Plugin registers functions to current env
    ↓
Functions available in script
```

### Using Plugins in Scripts

```javascript
// Load plugin dynamically
import("ta");
import("fin");

// Now plugin functions are available
let sma = ta.sma(CLOSE, 20);
let rsi = ta.rsi(CLOSE, 14);
let sharpe = strategy.sharpe(RETURNS, 0.02, 252);
```

**Real-world examples:**
```javascript
// cck_herding_model.tbs
import("fin");
import("ta");

function run_risk_check(returns, equity) {
    var var95 = strategy.var_hist(returns, 0.95);
    var dd = strategy.drawdown_stats(equity);
    // ... risk gating logic
}
```

---

## 📦 Creating a Plugin

### Plugin ABI (ts_plugin.h)

Every plugin DLL exports **exactly one function**:

```c
TS_EXPORT const ts_plugin_t *ts_api_create(void);
```

The `ts_plugin_t` structure:

```c
typedef struct ts_plugin_s {
    const char *name;       // Plugin name (e.g., "ta", "my_plugin")
    uint32_t    version;    // ABI version (currently 1)

    // Called when import("name") is executed
    void *(*load)(void *env, void *scratch);

    // Called when context is freed
    void (*unload)(void *instance);
} ts_plugin_t;
```

---

### Method 1: Stateless Plugin (TS_PLUGIN_MODULE)

**Use case:** Simple function registration, no state needed.

```c
// my_math_plugin.c
#include "ts_plugin.h"
#include "exprtk_module.h"
#include <math.h>

// Define your functions
static exprtk_value_t my_factorial(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 1 || args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    int n = (int)args[0].number;
    if (n < 0) return exprtk_value_number(NAN);

    double result = 1.0;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }

    return exprtk_value_number(result);
}

static exprtk_value_t my_fibonacci(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 1 || args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    int n = (int)args[0].number;
    if (n < 0) return exprtk_value_number(NAN);
    if (n <= 1) return exprtk_value_number(n);

    double a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        double temp = a + b;
        a = b;
        b = temp;
    }

    return exprtk_value_number(b);
}

// Define module
static exprtk_func_entry_t my_math_funcs[] = {
    {"factorial", my_factorial},
    {"fibonacci", my_fibonacci},
    {NULL, NULL}  // Sentinel
};

static const exprtk_module_t my_math_module = {
    .name = "my_math",
    .funcs = my_math_funcs
};

const exprtk_module_t *exprtk_module_my_math(void) {
    return &my_math_module;
}

// Export plugin (one-liner!)
TS_PLUGIN_MODULE(my_math, exprtk_module_my_math)
```

**What TS_PLUGIN_MODULE does:**
```c
// Expands to:
static void *ts__my_math_load(void *env, void *scratch) {
    const exprtk_module_t *mod = exprtk_module_my_math();
    exprtk_env_add_module((exprtk_env_t *)env, mod);
    return env;
}

static void ts__my_math_unload(void *inst) {
    // Nothing to do
}

static const ts_plugin_t g_my_math = {
    .name = "my_math",
    .version = 1,
    .load = ts__my_math_load,
    .unload = ts__my_math_unload,
};

TS_EXPORT const ts_plugin_t *ts_api_create(void) {
    return &g_my_math;
}
```

---

### Method 2: Stateful Plugin (TS_PLUGIN_STATEFUL)

**Use case:** Plugin needs to maintain state (e.g., database connection, cache).

```c
// sqlite_plugin.c
#include "ts_plugin.h"
#include <sqlite3.h>

// Plugin context
typedef struct {
    sqlite3 *db;
    // ... other state
} sqlite_ctx_t;

// Create context
static sqlite_ctx_t *sqlite_create(void) {
    sqlite_ctx_t *ctx = malloc(sizeof(sqlite_ctx_t));
    ctx->db = NULL;
    return ctx;
}

// Register functions
static void sqlite_register(sqlite_ctx_t *ctx, void *env, void *scratch) {
    // Register functions that use ctx
    exprtk_env_register_func(env, "db_open", my_db_open, ctx);
    exprtk_env_register_func(env, "db_query", my_db_query, ctx);
    exprtk_env_register_func(env, "db_close", my_db_close, ctx);
}

// Destroy context
static void sqlite_destroy(sqlite_ctx_t *ctx) {
    if (ctx->db) sqlite3_close(ctx->db);
    free(ctx);
}

// Export plugin
TS_PLUGIN_STATEFUL(sqlite, sqlite_create, sqlite_register, sqlite_destroy)
```

---

## 🛠️ Building a Plugin

### Windows (MSVC)

```bash
cl /LD my_math_plugin.c ^
   /I"C:\turbonet\tScript\ts_loader\include" ^
   /I"C:\turbonet\tScript\exprtk\include" ^
   /Fe:my_math.dll
```

### Linux (GCC)

```bash
gcc -shared -fPIC my_math_plugin.c \
    -I/path/to/tScript/ts_loader/include \
    -I/path/to/tScript/exprtk/include \
    -o my_math.so
```

### CMake

```cmake
add_library(my_math_plugin SHARED my_math_plugin.c)

target_include_directories(my_math_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/tScript/ts_loader/include
    ${CMAKE_SOURCE_DIR}/tScript/exprtk/include)

# Windows: auto-export symbols
set_target_properties(my_math_plugin PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
```

---

## 📂 Plugin Discovery

### Default Search Paths

TurboScript searches for plugins in these locations (in order):

1. **Executable plugins directory**: `<exe_dir>/plugins/my_plugin.dll`
2. **Executable directory compatibility fallback**: `<exe_dir>/my_plugin.dll`

The current working directory and the operating-system `PATH` are not searched.
This keeps plugin selection independent of the directory from which TurboScript
was launched and avoids loading an unintended same-named library.
If neither prefixless path can be opened, TurboScript retries the legacy
`tbs_my_plugin.dll` filename in the same two directories.

### Plugin Naming Convention

```
Windows: <name>.dll
Linux:   <name>.so
macOS:   <name>.dylib
```

**Examples:**
- `import("ta")` → searches for `ta.dll`
- `import("my_math")` → searches for `my_math.dll`

Two built-in plugins use collision-safe file stems because their dependencies
already own the logical-name library filenames:

- `import("crypto")` → `crypto_plugin.dll` (`crypto_plugin.so` on Linux)
- `import("rules_forge")` → `rules_forge_plugin.dll` (`rules_forge_plugin.so` on Linux)

Their logical names and plugin descriptor names remain `crypto` and
`rules_forge`.

---

## 🔧 C API for Plugin Management

### Load Plugin from C Code

```c
#include "turbo_script.h"

turbo_script_ctx_t *ctx = turbo_script_init_bare();

// Load plugin by name (searches plugin paths)
int result = turbo_script_load_plugin(ctx, "ta");
if (result != 0) {
    fprintf(stderr, "Failed to load ta plugin\n");
}

// Now ta functions are available
turbo_script_eval(ctx, "let sma = ta.sma(CLOSE, 20);");
```

### Manual Plugin Registration

```c
// Register plugin from specific path
int turbo_script_register_plugin(turbo_script_ctx_t *ctx, const char *path);

// Example
turbo_script_register_plugin(ctx, "C:/custom/my_plugin.dll");
```

---

## 📊 Plugin Lifecycle

```
┌─────────────────────────────────────────────────────────┐
│                   Plugin Lifecycle                       │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  1. import("plugin_name")                               │
│     ↓                                                    │
│  2. Search for plugin_name.dll                          │
│     ↓                                                    │
│  3. dlopen/LoadLibrary                                  │
│     ↓                                                    │
│  4. ts_api_create() → ts_plugin_t*                      │
│     ↓                                                    │
│  5. plugin->load(env, scratch) → instance               │
│     ↓                                                    │
│  6. Functions registered to env                         │
│     ↓                                                    │
│  7. Script uses plugin functions                        │
│     ↓                                                    │
│  8. turbo_script_free(ctx)                              │
│     ↓                                                    │
│  9. plugin->unload(instance)                            │
│     ↓                                                    │
│ 10. dlclose/FreeLibrary                                 │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

---

## 🎯 Built-in vs Plugin Comparison

| Feature | Built-in Modules | Plugin Modules |
|---------|------------------|----------------|
| **Loading** | Compile-time | Runtime (dynamic) |
| **Registration** | Global registry | Per-context env |
| **Import needed?** | ❌ No | ✅ Yes |
| **Overhead** | Zero | Minimal (dlopen) |
| **Distribution** | Part of turbo_script.dll | Separate DLLs |
| **Update** | Recompile exprtk | Replace DLL |
| **Use case** | Core functions | Domain-specific |

---

## 🚀 Real-World Plugin Examples

### Example 1: Technical Analysis Plugin

```c
// ta_plugin.c
#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_ta(void);

TS_PLUGIN_MODULE(ta, exprtk_module_ta)
```

**Usage:**
```javascript
import("ta");

let sma20 = ta.sma(CLOSE, 20);
let rsi14 = ta.rsi(CLOSE, 14);
let macd = ta.macd(CLOSE, 12, 26, 9);
```

---

### Example 2: Finance Plugin

```c
// fin_plugin.c (strategy module)
#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_strategy(void);

TS_PLUGIN_MODULE(fin, exprtk_module_strategy)
```

**Usage:**
```javascript
import("fin");

// Portfolio helper
let cov = [0.04, 0.01, 0.01, 0.09];
let weights = strategy.pf_min_variance(cov);

// Risk metrics
let var95 = strategy.var_hist(returns, 0.95);
let cvar95 = strategy.cvar(returns, 0.95);
```

---

### Example 3: Network Plugin

```c
// net_plugin.c
#include "ts_plugin.h"

typedef struct {
    // HTTP client state
    void *curl_handle;
} net_ctx_t;

static net_ctx_t *net_create(void) {
    net_ctx_t *ctx = malloc(sizeof(net_ctx_t));
    ctx->curl_handle = curl_easy_init();
    return ctx;
}

static void net_register(net_ctx_t *ctx, void *env, void *scratch) {
    exprtk_env_register_func(env, "http_get", my_http_get, ctx);
    exprtk_env_register_func(env, "http_post", my_http_post, ctx);
}

static void net_destroy(net_ctx_t *ctx) {
    curl_easy_cleanup(ctx->curl_handle);
    free(ctx);
}

TS_PLUGIN_STATEFUL(net, net_create, net_register, net_destroy)
```

**Usage:**
```javascript
import("net");

let response = net.http_get("https://api.example.com/data");
let json = parse_json(response);
```

---

## 🔒 Security Considerations

### Plugin Sandboxing

**Current implementation**: Native plugins are not sandboxed and run with the
host process's permissions. Embedders can install a name-based authorization
callback before context initialization so an unapproved dynamic library is
rejected before it is opened:

```c
#include "turbo_script.h"
#include <stdbool.h>
#include <string.h>

static bool allow_native_plugin(const char *name, void *user_data) {
    const char *const *allowlist = user_data;
    for (size_t i = 0; allowlist[i] != NULL; ++i) {
        if (strcmp(name, allowlist[i]) == 0) return true;
    }
    return false;
}

int main(void) {
    const char *allowlist[] = {"parser", "net", NULL};
    turbo_script_ctx_t *ctx = turbo_script_init_with_plugin_authorizer(
        TURBO_SCRIPT_INIT_DEFAULT, allow_native_plugin, allowlist);
    if (!ctx) return 1;

    int result = turbo_script_load_plugin(ctx, "net");
    turbo_script_free(ctx);
    return result == 0 ? 0 : 1;
}
```

The callback applies to the first native load for each logical plugin name.
Built-in modules such as `math` and `.tbs` script modules do not invoke it.
`TURBO_SCRIPT_INIT_DEFAULT` requests the native `parser` plugin, so an allowlist
for default contexts must include `parser`. The callback data is borrowed and
must outlive the context.

This is an admission-control hook, not a permission sandbox: it does not verify
signatures, constrain filesystem or network access, or isolate plugin crashes.

**Recommendations:**
1. Only load plugins from trusted sources
2. Verify plugin signatures
3. Use `turbo_script_init_with_plugin_authorizer()` with an explicit allowlist
4. Use process-level isolation when plugins must not inherit host permissions

---

## 🐛 Troubleshooting

### Plugin Not Found

```
Error: Failed to load plugin 'my_plugin'
```

**Solutions:**
1. Check the plugin exists as `<exe_dir>/plugins/my_plugin.dll`
2. Check that its dependent libraries are beside the plugin or in the application directory
3. Inspect the reported loader stage (`open`, `symbol`, `ABI`, or `initialize`)

---

### Symbol Not Found

```
Error: ts_api_create not found in my_plugin.dll
```

**Solutions:**
1. Ensure `TS_PLUGIN_MODULE` or `TS_PLUGIN_STATEFUL` macro is used
2. Check DLL exports: `dumpbin /EXPORTS my_plugin.dll` (Windows)
3. Verify `TS_EXPORT` is defined correctly

---

### Plugin Crashes

**Common causes:**
1. Memory corruption (use arena allocator)
2. Null pointer dereference
3. ABI mismatch (recompile plugin)

**Debug:**
```c
// Enable plugin debug logging
#define TS_PLUGIN_DEBUG 1
```

---

## 📚 Best Practices

### 1. Use Arena Allocator

```c
// ✅ GOOD: Use provided arena
static exprtk_value_t my_func(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, turbo_pool_t *arena) {
    double *temp = TURBO_POOL_ALLOC_ARRAY(arena, double, 100);
    // No need to free - arena handles it
}

// ❌ BAD: Manual malloc/free
static exprtk_value_t my_func(...) {
    double *temp = malloc(100 * sizeof(double));
    // Easy to leak!
    free(temp);
}
```

---

### 2. Error Handling

```c
static exprtk_value_t my_func(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, turbo_pool_t *arena) {
    // Validate arguments
    if (argc != 2) {
        return exprtk_value_number(NAN);  // Return NaN on error
    }

    if (args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    // ... implementation
}
```

---

### 3. Version Compatibility

```c
// Check TurboScript version
#define TS_API_VERSION 1

const exprtk_module_t *exprtk_module_my_plugin(void) {
    #if TS_API_VERSION < 1
    #error "This plugin requires TurboScript API version 1 or higher"
    #endif

    return &my_module;
}
```

---

## 📖 See Also

- **[FIN_MODULE.md](FIN_MODULE.md)** - Finance plugin documentation
- **[FACTORS.md](FACTORS.md)** - Factor processing guide
- **[PORTFOLIO.md](PORTFOLIO.md)** - Portfolio optimization guide
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - TurboScript architecture

---

**Built with ❤️ for TurboScript plugin developers**
