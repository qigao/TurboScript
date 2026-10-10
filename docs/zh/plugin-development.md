# 插件开发指南

TurboScript 的全部动态模块已统一采用 Salts Plugin 发布契约。脚本仍使用
`import("net")`、`http.*`、`ws.*` 等原有入口。net 的 HTTP/WebSocket 仍由
`CHttp::Client` 驱动；Plugin 只管理契约准入和 DLL 生命周期。

## 版本与迁移

所有插件必须使用当前 `ts_plugin.h` 和匹配的 SDK 重新构建。旧 `ts_api_create`
入口不再被加载。TurboScript module contract 为 2，当前 Salts Plugin ABI 为 5；
它们与 host module API 版本 1、SDK 包版本分别独立。回滚时同时恢复宿主及整套插件。

## 发布与构建

发布方链接 `Salts::PluginABI`，宿主链接 `Salts::Plugin`。当前工程的插件通过 ExprTk
继承发布依赖；安装包消费方链接 `TurboScript::ExprTk` 和 `Salts::PluginABI`。
依赖路径、平台及构建配置统一通过项目的 CMake user presets 选择。

无状态模块使用 `TS_PLUGIN_MODULE(name, module_fn)`，实际完整示例见
[插件系统文档](../PLUGIN_SYSTEM.md#publication)。有状态模块使用
`TS_PLUGIN_STATEFUL(name, create_fn, loader_fn, destroy_fn)`：create 返回 context，
loader 注册函数，destroy 完成全部清理且不能失败。

网络等清理可能失败的模块使用
`TS_PLUGIN_STATEFUL_CHECKED(name, create_fn, loader_fn, close_fn)`。
close 返回 0 表示实例已被消费；非零表示实例及未完成资源仍归原 owner，可重试。
每个 context 拥有独立 instance，不能把它放进 DSO 共享的静态 manifest self。

需要自定义初始化/回滚时，实现头文件已声明的
`ts_plugin_open(void *env, void *scratch)` 与 `ts_plugin_close(void *instance)`，
随后 `TS_PLUGIN_PUBLISH("name")`。open 成功返回须由 close 归还的逻辑拥有句柄，
失败返回 NULL，且必须先回滚部分初始化和注册。普通 stateful 宏的 loader 返回 void；
不能用它假装支持会失败的注册事务。

CMeta 从同一声明生成 Function metadata 和精确调用 adapter；宿主核对 export 的
contract ID、版本和 Function ABI 后调用。不自行复制签名表，不强转函数指针。
返回 instance 不代表宿主可以 free；stateless 模块允许用 env 地址作为逻辑句柄。

## 关闭与失败

每个加载 handle 拥有容量 1 的 Salts registry 和覆盖全部使用期的 lease；
一个 context 最多 16 个动态插件。停止 task/timer/JIT 调用、释放 env 借用值后，
先关闭新的 lease 准入，再在现有 lease 下执行 instance close。
清理成功才 release lease、检查 quiescence 并卸载 DLL。

`ts_plugin_unload_ex(handle, error)` 成功消费 handle；失败保留 handle 和未完成义务供重试。
instance close 失败时 instance 与 lease 均保留；已成功 close 的实例不会再次 close。
停止后不能重新初始化。NULL 可直接传入。
void `ts_plugin_unload()` 和 context 析构无法向调用方返回重试义务，清理失败时
采用 fail-fast；需要可重试行为的应用使用显式 loader close API。

net 对 HTTP/WS destroy 错误保留 client，不再直接 free 仍有义务的 owner。
已成功清理的 client 清空后不重复销毁。此变更不提供热重载、沙箱或新 I/O 调度器。

## 发现与验证

保留现有显式路径与 executable-relative 发现规则。Windows 继续使用受限依赖目录，
不从当前工作目录或 PATH 搜索插件。加载时先安全打开确定文件，再交给 Salts registry
准入并转交引用；不回退到旧 ABI。部署期间不能替换正在加载的文件。

正式测试为 `test_ts_plugin_loader`、`test_ts_builtin_plugins`、`test_net_ctx` 及
相邻脚本/模块回归。前两者覆盖不兼容拒绝、lease/close 重试和全部 11 个真实插件的
跨 context 生命周期。通过对应 user preset 的 CTest 执行。
公网 HTTP 用例需显式设置 `TS_NET_TESTS=1`；离线测试通过不能代表 HTTPS 公网验证通过。

详细协议、错误语义及示例见 [Plugin System](../PLUGIN_SYSTEM.md)、
[ts_plugin.h](../../exprtk/include/ts_plugin.h) 和
[设计决策](../superpowers/specs/2026-08-25-turboscript-host-module-abi-design.md)。
