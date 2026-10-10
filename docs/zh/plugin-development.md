# TurboScript 原生插件开发

TurboScript 原生插件统一使用 **当前 Salts Plugin ABI**。

TurboScript 不再拥有第二套通用原生插件 ABI。插件的 DSO 加载、生命周期、
lease 与 capability 发布由 Salts::Plugin 管理；原生函数和接口语义由 CMeta
描述。

## 架构

```text
import("fin")
   |
   v
TurboScript import / 命名空间
   |
   v
Salts::Plugin registry
  load -> start -> acquire lease
   |
   v
cmeta_plugin_manifest
   |
   +--> FUNCTION
   |      CMeta FunctionDesc
   |      CMeta FunctionAbi
   |      exact invoke/context
   |
   +--> INTERFACE
          CMeta InterfaceDesc
          { self, vtable }
   |
   v
TurboScript 已绑定 dispatch cache
   |
   +--> interpreter / JIT
   +--> 满足条件时进入 CFlow
```

职责边界：

- **Salts::Plugin**：DSO、唯一 Plugin ABI、生命周期、lease。
- **CMeta**：函数/接口类型、ABI、effects、properties、参数方向和所有权。
- **TurboScript**：`import()`、脚本命名空间、脚本值转换、诊断、解释器/JIT。
- **CFlow**：对满足 CMeta contract 的预绑定 Function 做图执行。

TurboScript 的 Host Module ABI 是宿主执行 TurboScript 程序的 ABI，与原生
Plugin publication ABI 是两回事。

## 加载与发现

```javascript
import("fin");
```

TurboScript 根据平台解析插件文件，然后通过
`cmeta_plugin_registry_*` 加载。DSO 必须发布当前 ABI 的
`cmeta_plugin_query`，并返回合法 manifest。

典型文件名：

```text
Windows  fin.dll
Linux    fin.so
macOS    fin.dylib
```

加载器使用可执行文件/安装包插件目录，不搜索当前工作目录。部分仓库插件因
依赖库重名使用避碰物理文件名，但逻辑 plugin ID 不变。

## Canonical Function export

普通无状态原生操作优先发布为 FUNCTION。

语义真相只有：

```text
FunctionDesc + FunctionAbi + exact invoke adapter
```

TurboScript 中的 dispatch row 只是缓存，不是第二份签名/effect 描述。

示例：

```c
#include <salts/plugin.h>
#include <salts/thread.h>

FunctionDeclResult(value, double, CMETA_RESULT_VALUE, my_double,
    (double, value, CMETA_PARAM_IN));

double my_double(double value) {
    return value * 2.0;
}

static bool CMETA_PLUGIN_CALL my_double_invoke(
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

static cmeta_plugin_export exports[1];
static salts_once_t exports_once = SALTS_ONCE_INIT;

static void init_exports(void) {
    exports[0] = (cmeta_plugin_export){
        .struct_size = CMETA_PLUGIN_EXPORT_SIZE,
        .kind = CMETA_PLUGIN_EXPORT_FUNCTION,
        .contract_version = 1u,
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

static const cmeta_plugin_manifest manifest = {
    .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,
    .abi_version = CMETA_PLUGIN_ABI_VERSION,
    .plugin_id = "my_math",
    .version = {1u, 0u, 0u},
    .exports = exports,
    .export_count = 1u,
};

CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL
cmeta_plugin_query(uint32_t host_abi) {
    if (host_abi != CMETA_PLUGIN_ABI_VERSION) return NULL;
    salts_once(&exports_once, init_exports);
    return &manifest;
}
```

脚本名称由 `export_id` 决定：

```javascript
import("my_math");
value = math.double(3.5);
```

### 当前 scalar binding 范围

TurboScript 当前直接支持 CMeta finite scalar universe：

- `bool`
- `int`
- `long`
- `float`
- `double`
- 0–16 个 `IN` scalar 参数
- scalar 或 `void` 返回值

pointer/object/aggregate/opaque 以及 OUT/INOUT 不能靠动态猜测调用；需要明确的
后续 language binding。

对 canonical integer binding，TurboScript 保留原始 ExprTk value 类型，
不会先做旧的 integer→double 归一化，因此 `long` ABI 不会在进入 adapter
之前丢失 int64 精度。

## CFlow

Plugin Function 和其它 graphable callable 使用同一条 CMeta/CFlow admission
路径。

PURE 且类型/ABI 合法的 Function 可以进入 CFlow。FunctionDesc 提供
effects/properties，FunctionAbi 提供 native carrier contract。

```text
Plugin Function
  -> live lease
  -> FunctionDesc / FunctionAbi
  -> pre-bound exact adapter
  -> CFlow projection
  -> Graph / Plan
```

STATEFUL、IO、ASYNC、UNKNOWN 等 effect 会形成保守 barrier。TurboScript
不会维护单独的插件 purity/effect 表。

只要 Graph/Plan/run 仍能访问插件代码、descriptor、trait、context 或
Interface，Plugin lease 就必须保持有效。

## Canonical Interface export

长生命周期、有状态、多态 provider 应发布 INTERFACE：

```text
CMeta InterfaceDesc + { self, vtable }
```

典型对象：

- 数据库/resource provider
- 网络/session provider
- device/runtime provider

Interface 是领域原生 contract。TurboScript adapter 负责脚本值转换，不应把
ExprTk 类型泄漏进领域 Interface。

数据库能力统一通过 TurboDB::Orm + 用户显式 driver/module 配置；#84 跟踪该集成，不在 TurboScript 中再定义 SQLite provider。

## 仓库中的迁移 helper

当前仓库仍保留：

- `TS_PLUGIN_MODULE`
- `TS_PLUGIN_MODULE_WITH_UNLOAD`
- `TS_PLUGIN_STATEFUL`

它们已经通过当前 Salts Plugin manifest 发布 capability，只用于逐步迁移旧
模块；**不是**另一套 loader 或 ABI。

新 capability 的默认原则：

1. 普通操作发布 canonical FUNCTION；
2. 有状态 provider 发布 canonical INTERFACE；
3. 已有逻辑 Service/Component contract 时复用 DataBind/CMeta 生成结果；
4. TurboScript 只保留脚本命名空间和转换层。

## Hot path

Plugin discovery/reflection 属于 control plane。

完成 import/binding 后，调用只使用预绑定 exact adapter 或 provider vtable，
不得每次调用重新：

- 查 Plugin registry
- 查 DSO symbol
- 查 manifest
- 协商 ABI
- 查 reflection catalog

## 生命周期

TurboScript context 在插件 callback、descriptor、Interface 或 CFlow callable
仍可达期间持有 Salts Plugin lease。

销毁 context 时，先销毁脚本侧引用，再 release lease，最后停止/卸载 DSO。

内置 net 保留当前 `void unload` Interface 契约。脚本调用 `ws.close()` 时，
如果 CHttp destroy 失败，会返回 `0` 并保留 client，允许再次关闭；重新连接也
不能覆盖销毁失败的 client。context 最终销毁无法向调用方返回清理失败，
因此 drain 失败时会终止进程，避免释放仍存活的资源或继续卸载插件。

DataBind Service 的输出由 BindingPlan 的 begin/write/commit/abort 事务管理。
参数校验失败前不创建结果 map；拒绝非零参数数量配合空参数存储。
原生值编码使用 provider 给出的实际存储长度，由 DataBind 验证描述符与存储
是否匹配。连续调用的结果各自拥有其 map，不共享可变的响应暂存区。

## 构建

插件应直接使用安装好的/current Salts Plugin 与 CMeta headers，不复制 ABI
struct。

本仓库 CMake 使用 `Salts::Plugin` / `Salts::PluginABI` 等正常依赖图。

## 故障排除

### open 阶段失败

检查插件文件是否位于配置的插件/安装包目录，并确认其动态依赖可用。

### symbol/ABI 阶段失败

确认 DSO 发布 `cmeta_plugin_query`，并使用当前 Salts Plugin ABI 重新构建。

不存在旧 TurboScript 私有 Plugin ABI fallback。

### Function 初始化失败

检查：

- FunctionDesc / FunctionAbi 是否合法；
- exact adapter 是否存在；
- 参数 direction/carrier 是否属于当前 TurboScript binding 范围；
- `export_id` 是否与已有 env function 冲突。

## 相关内容

- `exprtk/include/ts_plugin.h`：基于 Salts Plugin Interface 的迁移 helper。
- `turbo_script/include/ts_plugin_loader.h`：TurboScript binding adapter。
- #22：Plugin ABI/CMeta 总体收敛。
- #84：TurboDB::Orm 数据库集成与显式用户 driver 配置。
