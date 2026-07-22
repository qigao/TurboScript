# TurboScript GoF 示例

这些示例展示设计模式如何解决具体变化点，而不是追求把 GoF 23 种模式全部堆进同一套对象层级。每个脚本都没有外部依赖，并在结束前进行结果校验；校验失败会直接抛出错误。

| 示例 | 模式 | 适用边界 |
|------|------|----------|
| `factory_builder.tbs` | Factory、Builder | 创建实现需要按配置选择；复杂对象需要分步校验 |
| `adapter_decorator.tbs` | Adapter、Decorator | 隔离旧接口；按需组合输出职责 |
| `composite_visitor.tbs` | Composite、Visitor | 递归树结构稳定，而统计操作需要独立扩展 |
| `command_memento.tbs` | Command、Memento | 操作需要记录、撤销和重做 |
| `state_chain.tbs` | State、Chain of Responsibility | 状态转换与输入校验链分别变化 |

运行单个示例：

```powershell
.\build\Msvc\bin\turbo_script_repl.exe --file .\examples\gof\factory_builder.tbs
```

设计约束：

- 示例通过构造函数显式注入依赖，不使用 Singleton 或 Service Locator。
- Composite 的父节点拥有子节点列表；Visitor 只持有本次遍历的派生统计状态。
- Command 保存执行目标和 Memento；History 是命令顺序与游标的唯一事实源。
- Decorator 只包装同一接口，不承担对象创建或业务校验。

