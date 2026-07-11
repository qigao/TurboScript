# 插件开发指南

学习如何使用 C/C++ 扩展 TurboScript。

---

## 目录

1. [快速开始](#快速开始)
2. [插件架构](#插件架构)
3. [创建简单插件](#创建简单插件)
4. [有状态插件](#有状态插件)
5. [构建插件](#构建插件)
6. [插件发现](#插件发现)
7. [最佳实践](#最佳实践)
8. [故障排除](#故障排除)

---

## 快速开始

### 5 分钟插件

用 3 步创建一个简单的数学插件：

**步骤 1：编写插件** (`my_math_plugin.c`)：

```c
#include "ts_plugin.h"
#include "exprtk_module.h"

// 定义你的函数
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

// 注册函数
static exprtk_func_entry_t my_math_funcs[] = {
    {"factorial", my_factorial},
    {NULL, NULL}  // 哨兵
};

static const exprtk_module_t my_math_module = {
    .name = "my_math",
    .funcs = my_math_funcs
};

const exprtk_module_t *exprtk_module_my_math(void) {
    return &my_math_module;
}

// 导出插件（一行搞定！）
TS_PLUGIN_MODULE(my_math, exprtk_module_my_math)
```

**步骤 2：构建插件**：

```bash
# Windows (MSVC)
cl /LD my_math_plugin.c /I"path/to/tScript/include" /Fe:tbs_my_math.dll

# Linux (GCC)
gcc -shared -fPIC my_math_plugin.c -I"path/to/tScript/include" -o tbs_my_math.so
```

**步骤 3：在 TurboScript 中使用**：

```javascript
import("my_math");

var result = my_math.factorial(5);  // 120
print(result);
```

完成！🎉

---

## 插件架构

### 双层模块系统

TurboScript 使用双模块系统：

```
┌─────────────────────────────────────────────────────────────┐
│                    TurboScript 运行时                        │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────────┐      ┌──────────────────────┐   │
│  │   内置模块           │      │   插件模块           │   │
│  │  (编译时)            │      │  (运行时加载)        │   │
│  ├──────────────────────┤      ├──────────────────────┤   │
│  │ • math               │      │ • tbs_ta.dll         │   │
│  │ • string             │      │ • tbs_fin.dll        │   │
│  │ • stats              │      │ • tbs_net.dll        │   │
│  │ • io                 │      │ • tbs_sqlite.dll     │   │
│  │ • core               │      │ • tbs_custom.dll     │   │
│  └──────────────────────┘      └──────────────────────┘   │
│           ↓                              ↓                  │
│  全局注册表                        每上下文环境              │
│  (exprtk_registry)               (exprtk_env_t)            │
└─────────────────────────────────────────────────────────────┘
```

**内置模块**：编译到 `exprtk.dll` 中，始终可用，无需 `import()`。

**插件模块**：动态加载的 DLL，通过 `import("name")` 按需加载。

---

## 插件 ABI

每个插件 DLL 导出**恰好一个函数**：

```c
TS_EXPORT const ts_plugin_t *ts_api_create(void);
```

`ts_plugin_t` 结构：

```c
typedef struct ts_plugin_s {
    const char *name;       // 插件名称（例如 "ta"、"my_plugin"）
    uint32_t    version;    // ABI 版本（当前为 1）

    // 当执行 import("name") 时调用
    void *(*load)(void *env, void *scratch);

    // 当上下文被释放时调用
    void (*unload)(void *instance);
} ts_plugin_t;
```

---

## 创建简单插件

### 方法 1：无状态插件（TS_PLUGIN_MODULE）

**用例**：简单的函数注册，不需要状态。

```c
#include "ts_plugin.h"
#include "exprtk_module.h"
#include <math.h>

// 定义你的函数
static exprtk_value_t my_square(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 1 || args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    double x = args[0].number;
    return exprtk_value_number(x * x);
}

static exprtk_value_t my_cube(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 1 || args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    double x = args[0].number;
    return exprtk_value_number(x * x * x);
}

// 定义模块
static exprtk_func_entry_t my_funcs[] = {
    {"square", my_square},
    {"cube", my_cube},
    {NULL, NULL}  // 哨兵
};

static const exprtk_module_t my_module = {
    .name = "my_plugin",
    .funcs = my_funcs
};

const exprtk_module_t *exprtk_module_my_plugin(void) {
    return &my_module;
}

// 导出插件（一行搞定！）
TS_PLUGIN_MODULE(my_plugin, exprtk_module_my_plugin)
```

---

## 有状态插件

### 方法 2：TS_PLUGIN_STATEFUL

**用例**：插件需要维护状态（例如数据库连接、缓存）。

```c
#include "ts_plugin.h"
#include <sqlite3.h>

// 插件上下文
typedef struct {
    sqlite3 *db;
    int connection_count;
} sqlite_ctx_t;

// 创建上下文
static sqlite_ctx_t *sqlite_create(void) {
    sqlite_ctx_t *ctx = malloc(sizeof(sqlite_ctx_t));
    ctx->db = NULL;
    ctx->connection_count = 0;
    return ctx;
}

// 使用上下文的函数
static exprtk_value_t db_open(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, turbo_pool_t *arena) {
    sqlite_ctx_t *ctx = (sqlite_ctx_t *)env->user_data;

    if (argc != 1 || args[0].type != EXPRTK_STRING) {
        return exprtk_value_number(0);
    }

    const char *path = args[0].data.string.value;
    int rc = sqlite3_open(path, &ctx->db);

    if (rc == SQLITE_OK) {
        ctx->connection_count++;
        return exprtk_value_number(1);
    }

    return exprtk_value_number(0);
}

// 注册函数
static void sqlite_register(sqlite_ctx_t *ctx, void *env, void *scratch) {
    exprtk_env_t *e = (exprtk_env_t *)env;

    // 在环境中存储上下文
    e->user_data = ctx;

    // 注册函数
    exprtk_env_register_func(e, "sqlite.open", db_open, NULL);
    exprtk_env_register_func(e, "sqlite.query", db_query, NULL);
    exprtk_env_register_func(e, "sqlite.close", db_close, NULL);
}

// 销毁上下文
static void sqlite_destroy(sqlite_ctx_t *ctx) {
    if (ctx->db) {
        sqlite3_close(ctx->db);
    }
    free(ctx);
}

// 导出插件
TS_PLUGIN_STATEFUL(sqlite, sqlite_create, sqlite_register, sqlite_destroy)
```

**在 TurboScript 中使用**：

```javascript
import("sqlite");

sqlite.open("data.db");
var result = sqlite.query("SELECT * FROM users");
sqlite.close();
```

---

## 构建插件

### Windows (MSVC)

```bash
cl /LD my_plugin.c ^
   /I"C:\turbonet\tScript\ts_loader\include" ^
   /I"C:\turbonet\tScript\exprtk\include" ^
   /Fe:tbs_my_plugin.dll
```

### Linux (GCC)

```bash
gcc -shared -fPIC my_plugin.c \
    -I/path/to/tScript/ts_loader/include \
    -I/path/to/tScript/exprtk/include \
    -o tbs_my_plugin.so
```

### macOS (Clang)

```bash
clang -shared -fPIC my_plugin.c \
      -I/path/to/tScript/ts_loader/include \
      -I/path/to/tScript/exprtk/include \
      -o tbs_my_plugin.dylib
```

### CMake

```cmake
add_library(my_plugin SHARED my_plugin.c)

target_include_directories(my_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/tScript/ts_loader/include
    ${CMAKE_SOURCE_DIR}/tScript/exprtk/include)

# Windows：自动导出符号
set_target_properties(my_plugin PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 输出名称：tbs_my_plugin.dll/so
set_target_properties(my_plugin PROPERTIES
    OUTPUT_NAME "tbs_my_plugin")
```

---

## 插件发现

### 默认搜索路径

TurboScript 按以下顺序搜索插件：

1. **当前目录**：`./tbs_my_plugin.dll`
2. **插件子目录**：`./plugins/tbs_my_plugin.dll`
3. **系统插件目录**：`<install_dir>/plugins/tbs_my_plugin.dll`

### 插件命名约定

```
Windows: tbs_<name>.dll
Linux:   tbs_<name>.so
<name>_plugin.dylib  (macOS)
```

**示例：**
- `import("ta")` → 搜索 `tbs_ta.dll`
- `import("my_math")` → 搜索 `tbs_my_math.dll`

---

## 最佳实践

### 1. 使用 Arena 分配器

```c
// ✅ 好：使用提供的 arena
static exprtk_value_t my_func(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, turbo_pool_t *arena) {
    double *temp = TURBO_POOL_ALLOC_ARRAY(arena, double, 100);
    // 无需释放 - arena 会处理
}

// ❌ 差：手动 malloc/free
static exprtk_value_t my_func(...) {
    double *temp = malloc(100 * sizeof(double));
    // 容易泄漏！
    free(temp);
}
```

### 2. 验证参数

```c
static exprtk_value_t my_func(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, turbo_pool_t *arena) {
    // 检查参数数量
    if (argc != 2) {
        return exprtk_value_number(NAN);
    }

    // 检查参数类型
    if (args[0].type != EXPRTK_NUMBER || args[1].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    // ... 实现
}
```

### 3. 优雅地处理错误

```c
static exprtk_value_t divide(size_t argc, exprtk_value_t *args,
                             exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 2) return exprtk_value_number(NAN);

    double a = args[0].number;
    double b = args[1].number;

    // 检查除零
    if (fabs(b) < 1e-15) {
        return exprtk_value_number(NAN);  // 错误时返回 NaN
    }

    return exprtk_value_number(a / b);
}
```

---

## 故障排除

### 找不到插件

```
错误：无法加载插件 'my_plugin'
```

**解决方案：**
1. 检查插件文件是否存在：`tbs_my_plugin.dll`
2. 验证插件在搜索路径中
3. 使用绝对路径：`turbo_script_register_plugin(ctx, "C:/full/path/my_plugin.dll")`

### 找不到符号

```
错误：在 tbs_my_plugin.dll 中找不到 ts_api_create
```

**解决方案：**
1. 确保使用了 `TS_PLUGIN_MODULE` 或 `TS_PLUGIN_STATEFUL` 宏
2. 检查 DLL 导出：`dumpbin /EXPORTS tbs_my_plugin.dll`（Windows）
3. 验证 `TS_EXPORT` 定义正确

### 插件崩溃

**常见原因：**
1. 内存损坏（使用 arena 分配器）
2. 空指针解引用
3. ABI 不匹配（重新编译插件）

---

## 真实示例

### 示例：HTTP 客户端插件

```c
#include "ts_plugin.h"
#include <curl/curl.h>

typedef struct {
    CURL *curl;
} http_ctx_t;

static http_ctx_t *http_create(void) {
    http_ctx_t *ctx = malloc(sizeof(http_ctx_t));
    ctx->curl = curl_easy_init();
    return ctx;
}

static exprtk_value_t http_get(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, turbo_pool_t *arena) {
    http_ctx_t *ctx = (http_ctx_t *)env->user_data;

    if (argc != 1 || args[0].type != EXPRTK_STRING) {
        return exprtk_value_string("");
    }

    const char *url = args[0].data.string.value;

    // 执行 HTTP GET
    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    // ...（实现细节）

    return exprtk_value_string(response);
}

static void http_register(http_ctx_t *ctx, void *env, void *scratch) {
    exprtk_env_t *e = (exprtk_env_t *)env;
    e->user_data = ctx;
    exprtk_env_register_func(e, "http.get", http_get, NULL);
}

static void http_destroy(http_ctx_t *ctx) {
    curl_easy_cleanup(ctx->curl);
    free(ctx);
}

TS_PLUGIN_STATEFUL(http, http_create, http_register, http_destroy)
```

**使用：**
```javascript
import("http");

var response = http.get("https://api.example.com/data");
print(response);
```

---

## 另请参阅

- **[语言指南](language-guide.md)** - TurboScript 语法参考
- **[API 参考](api-reference.md)** - 内置函数
- **[架构](../advanced/architecture.md)** - 内部设计

---

**为 TurboScript 插件开发者用 ❤️ 构建**
