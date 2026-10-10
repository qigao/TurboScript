# TurboScript.Native

嵌入式 C/C++ SDK，保留 MIR 解释器和 JIT，包含原生扩展模块（含 Praktor 需要的 os/net），不附带 CLI。
SDK 目录：`sdk/linux-x64`、`sdk/macos-arm64`、`sdk/android-arm64-v8a`。
依赖 GitHub 发布的 `Salts.Native`、`SaltsUtils.Native`、`CHttp.Native`，通过 `*-*` 解析最新版本（包括 RC），不在 consumer 侧写死版本号。Salts 2.3 / SaltsUtils 4.3 / CHttp 2.1 的 SDK 必须一起重建和部署；旧 TurboScript 3.0.9 Unix 包依赖 Salts `.2`，不能与新 `.2.3` 动态库混用。

还原包后设置对应平台的 `SALTS_ROOT`、`SALTS_UTILS_ROOT`、`CHTTP_ROOT` 和 `TURBOSCRIPT_ROOT`。
通过 `find_package(TurboScript CONFIG REQUIRED PATHS "$ENV{TURBOSCRIPT_ROOT}" NO_DEFAULT_PATH)`
加载包，并链接 `TurboScript::TurboScript`。`CMAKE_PREFIX_PATH` 只指向匹配配置的 vcpkg 前缀；
第三方依赖继续由共享 vcpkg 工具链和项目 manifest 提供。

Linux/macOS 验证重新解包后的 C 与 C++ 消费端解释执行、JIT 执行、os/net 模块加载和结果值。
Android 使用 NDK API 26、c++_shared，验证 C/C++ 消费端交叉链接；
不把交叉编译成功当作设备运行验证。应用需自行部署 libc++_shared.so 和依赖动态库。

正式发布采用与 Salts 相同的 tag contract：`CMakeLists.txt`、root `vcpkg.json`、
公开头文件、Git tag 和 NuGet package 使用同一个版本。本版本为 `3.0.10`，只有
`v3.0.10` tag 可以发布 `TurboScript.Native 3.0.10`；master push 只做 qualification。

3.0.10 对齐 Salts 2.3 / SaltsUtils 4.3 / CHttp 2.1 的预发布依赖，修正测试 fixture 的显式 codec 生成，并为 CI 启用持久化 ccache。公开 C ABI 3 和 Host ABI 1 保持不变；升级时需一并部署匹配的依赖 SDK。

3.0.9 修复 net 资源清理的所有权与失败路径，并收紧 DataBind 调用的结果清理和存储边界。
发布构建使用 `vcpkg-cache` 的 `mir-jit-1.0.0-port3`，保留现有 SDK 平台范围。

3.0.8 适配当前 Salts / SaltsUtils 的 CMeta、协程 executor 和 plugin API；依赖仍浮动选择最新版本。
TLS 使用 CNet 提供的 GmSSL 实现，TurboScript 不链接 Lua / QuickJS。
使用旧 Salts plugin API 的原生插件需迁移到当前 CMeta plugin API，并与当前 SDK 一起重新编译。
Plugin Function 的 canonical non-void scalar result 继续显式发布 `CMETA_RESULT_VALUE`；
raw ExprTk builtin reflection 不从 C return spelling、ABI carrier 或 raw pointer 猜 ownership，
只在 contract 明确时发布 authoritative result semantics；typed text CFlow kernels 则统一发布
VALUE result semantics，让 CFlow/consumer 不再维护第二套 result-ownership 解释。
Plugin lease / manifest / export / interface lifetime boundary保持不变；TurboScript public C ABI 为 3，Host ABI 为 1。

3.0.6 收紧 shared-library export boundary：`TurboScript::TurboScript` 只导出
`turbo_script.h` 声明的 public C ABI，不再泄漏 PRIVATE MIR/ExprTk/Salts 符号。
这避免消费者进程中另一套 MIR/static dependency 通过 ELF/Mach-O symbol interposition
污染 TurboScript 自己的运行时，同时保留 3.0.5 的 DataBind Service Plugin binding、
多段 qualified native call、cooperative interrupt 和现有 import/plugin/.tbs 语义。

PR/master qualification 使用唯一的 `3.0.10-ci.<run>.<attempt>` 构建版本，但只作为
CI artifact，不推送到 GitHub Packages。每个 SDK 内的 manifest 记录实际源码提交、
依赖版本和构建配置。

## TurboDB / database module

The optional native `db` module is backed by the current TurboDB ORM package. Consumers that use `import("db")` should reference the latest `TurboDB.Native` package directly, just as TurboScript CI restores the latest GitHub release; TurboScript does not pin a TurboDB version or bundle a private ORM/driver loader.

The release includes the `db` module on Linux and Android. The macOS SDK excludes it because TurboDB does not currently publish a macOS SDK. Set `TURBODB_ROOT` to the matching restored SDK when using the database module.

Database choice is runtime user configuration. `db.connect({...})` requires an explicit canonical driver ID and an explicit driver module path. The module passes backend-specific `options` to TurboDB unchanged. It does not scan driver directories, choose SQLite by default, infer aliases, retry older ABIs, or fall back to another database. Driver modules remain TurboDB package artifacts and are loaded only when requested by user configuration.
