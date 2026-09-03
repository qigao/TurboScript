# TurboScript 标准库状态

本文档记录当前 TurboScript 公共模块的实现边界。结论以源码、CMake
目标和测试注册为准。

## 核心能力

- `core`：表达式、字符串、集合、函数和运行时类型。
- `mapper`：基于 TurboScript class 的 JSON/YAML/XML 映射。
- `os`：进程、服务、日志、电源和 Cron 能力。
- `io`、`net`、`sqlite`、`ta`、`fin`、`strategy`、`vec`：按各模块文档提供能力。

## Mapper

`mapper` 使用 class 字段声明作为唯一类型来源，并调用 Salts
Salts DataBind parser APIs 解析和生成文档：

- `mapper.read_json(Class, text)` / `mapper.write_json(instance)`
- `mapper.read_yaml(Class, text)` / `mapper.write_yaml(instance)`
- `mapper.read_xml(Class, text)` / `mapper.write_xml(instance)`

当前覆盖 `string`、`int`/`int64`、`number`、`bool`、`map`、`object`、`list`
和嵌套 class。JSON/YAML 未声明字段、类型不匹配值会快速失败，缺失字段
保留 class 默认值。

TurboScript 不提供独立 schema、codec、TBE 或 DataBind 公共接口。

## 验证状态

Mapper 测试目标为 `test_mapper_module`，注册的 CTest 名称为
`mapper_module_tests`。构建和测试应使用仓库 preset：

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R mapper --output-on-failure
```
