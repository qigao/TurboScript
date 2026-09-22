# TurboScript.Native

嵌入式 C/C++ SDK，保留 MIR 解释器和 JIT，不附带 CLI 与原生扩展模块。
SDK 目录：`sdk/linux-x64`、`sdk/macos-arm64`、`sdk/android-arm64-v8a`。
依赖 NuGet 包 Salts.Native 1.2.0 和 SaltsUtils.Native 2.0.2。

还原包后设置 `SALTS_ROOT`、`SALTS_UTILS_ROOT` 和包含三个 SDK 目录的
`CMAKE_PREFIX_PATH`，使用 `find_package(TurboScript CONFIG REQUIRED)` 与
`TurboScript::TurboScript`。第三方依赖继续由共享 vcpkg 工具链提供。

Linux/macOS 验证重新解包后的 C 与 C++ 消费端解释执行、JIT 执行和结果值。
Android 使用 NDK API 26、c++_shared，验证 ELF 架构及 C/C++ 消费端交叉链接；
不把交叉编译成功当作设备运行验证。应用需自行部署 libc++_shared.so 和依赖动态库。

版本从产品版本派生为唯一 `-ci.<run>.<attempt>` 预发布版本。每个 SDK 内的
manifest 记录实际源码提交、依赖版本和构建配置。本包不代表未合并 Host ABI PR 的验收。
