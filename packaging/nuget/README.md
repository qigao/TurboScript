# TurboScript.Native

嵌入式 C/C++ SDK，保留 MIR 解释器和 JIT，包含原生扩展模块（含 Praktor 需要的 os/net），不附带 CLI。
SDK 目录：`sdk/linux-x64`、`sdk/macos-arm64`、`sdk/android-arm64-v8a`。
依赖 GitHub 发布的 `Salts.Native`、`SaltsUtils.Native`、`CHttp.Native`，消费时始终选择最新可用版本，不在 consumer 侧写死任何版本号。

还原包后设置 `SALTS_ROOT`、`SALTS_UTILS_ROOT`、`CHTTP_ROOT`、`TURBOSCRIPT_ROOT` 和包含各 SDK 目录的
`CMAKE_PREFIX_PATH`，使用 `find_package(TurboScript CONFIG REQUIRED)` 与
`TurboScript::TurboScript`。第三方依赖继续由共享 vcpkg 工具链提供。

Linux/macOS 验证重新解包后的 C 与 C++ 消费端解释执行、JIT 执行、os/net 模块加载和结果值。
Android 使用 NDK API 26、c++_shared，验证 ELF 架构及 C/C++ 消费端交叉链接；
不把交叉编译成功当作设备运行验证。应用需自行部署 libc++_shared.so 和依赖动态库。

正式发布采用与 Salts 相同的 tag contract：`CMakeLists.txt`、root `vcpkg.json`、
Git tag 和 NuGet package 使用同一个版本。当前待发布版本为 `3.0.6`，只有
`v3.0.6` tag 可以发布 `TurboScript.Native 3.0.6`；master push 只做 qualification。

3.0.6 收紧 shared-library export boundary：`TurboScript::TurboScript` 只导出
`turbo_script.h` 声明的 public C ABI，不再泄漏 PRIVATE MIR/ExprTk/Salts 符号。
这避免消费者进程中另一套 MIR/static dependency 通过 ELF/Mach-O symbol interposition
污染 TurboScript 自己的运行时，同时保留 3.0.5 的 DataBind Service Plugin binding、
多段 qualified native call、cooperative interrupt 和现有 import/plugin/.tbs 语义。

PR/master qualification 使用唯一的 `3.0.6-ci.<run>.<attempt>` 构建版本，但只作为
CI artifact，不推送到 GitHub Packages。每个 SDK 内的 manifest 记录实际源码提交、
依赖版本和构建配置。

## TurboDB / database module

The optional native `db` module is backed by the current TurboDB ORM package. Consumers that use `import("db")` should reference the latest `TurboDB.Native` package directly, just as TurboScript CI restores the latest GitHub release; TurboScript does not pin a TurboDB version or bundle a private ORM/driver loader.

Database choice is runtime user configuration. `db.connect({...})` requires an explicit canonical driver ID and an explicit driver module path. The module passes backend-specific `options` to TurboDB unchanged. It does not scan driver directories, choose SQLite by default, infer aliases, retry older ABIs, or fall back to another database. Driver modules remain TurboDB package artifacts and are loaded only when requested by user configuration.

