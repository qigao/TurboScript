# TurboScript 文档索引

## 📚 核心文档

### 语言与 API

- **[TurboScript 语言指南](./language-guide.md)**  
  脚本语言语法、类型系统、控制流、OOP 特性

- **[TurboScript API 参考](../turbo_script/include/turbo_script.h)**  
  C API 接口、JIT 编译、变量绑定、插件系统

- **[Timer 与 Cron 调度](./advanced/timers.md)**
  `after`、固定延迟 interval、cron、executor 线程边界与关闭语义

- **[调度型 Task](./advanced/tasks.md)**
  CoroNet 托管 task、yield/sleep/join、并发 HTTP、结果与关闭协议

- **[Task runtime 与协作式取消](./architecture/task-runtime-cancellation.md)**
  task/timer/HTTP/REPL 的状态归属、token 生命周期与关闭顺序

### DataBind 模块

- **[DataBind 完整指南](./DATA_BIND_GUIDE.md)** ⭐ **推荐起点**  
  快速开始、常见问题、工具调试

- **[DataBind C API 文档](../tbe/data_bind/README.md)**  
  完整 C API 参考、错误处理、安全说明、性能配置

- **[DataBind 最佳实践](./data_bind_best_practices.md)**  
  性能优化、架构模式、错误处理、常见陷阱

---

## 💡 示例代码

### 基础示例

- **[运行时内存策略](advanced/memory-policy.md)**
  Context/task/external 配额、所有权边界与 callback-scoped WebSocket 消费

- **[Lambda 和闭包](../examples/test_lambda.tbs)**  
  箭头函数、高阶函数、闭包、IIFE

- **[IO 函数](../examples/test_io_functions.tbs)**  
  日期时间、文件操作、目录遍历

- **[周期检查网站](../examples/timer_website_watch.tbs)**
  使用 `timer.every` 和 `http.get` 定期检查网站

- **[并发 HTTP Task](../examples/task_parallel_http.tbs)**
  使用 `task.spawn` 和 `task.join` 并发等待多个 HTTP 请求

- **[Polymarket 多频道 Task](../examples/polymarket_multi_channel.tbs)**
  使用独立 managed task 对 market（多个资产）与 sports WebSocket 频道做一次接收 smoke test

- **[Polymarket 多频道 Long Run](../examples/polymarket_multi_channel_long_run.tbs)**
  在运行时限、事件数与重连次数配额内持续接收，采用同步 Observer 背压和采样输出

### GoF 设计模式示例

- **[GoF 示例导航](../examples/gof/README.md)**
  五个可独立运行并带自检的示例，覆盖 Factory、Builder、Adapter、Decorator、Composite、Visitor、Command、Memento、State 与 Chain of Responsibility

- **[Factory 与 Builder](../examples/gof/factory_builder.tbs)**
  隔离实现选择，并集中校验分步构造参数

- **[Adapter 与 Decorator](../examples/gof/adapter_decorator.tbs)**
  统一遗留行情接口，并动态组合渲染职责

- **[Composite 与 Visitor](../examples/gof/composite_visitor.tbs)**
  统一遍历文件树，并独立扩展统计操作

- **[Command 与 Memento](../examples/gof/command_memento.tbs)**
  对象化账户操作，并支持撤销与重做

- **[State 与责任链](../examples/gof/state_chain.tbs)**
  分离订单状态迁移与可组合输入校验

### DataBind 示例

- **[类包装模式](../examples/test_data_bind_class_wrapping.tbs)**  
  Plain Object → Class Instance 的三种包装模式

- **[交易系统](../examples/data_bind_trading_system.tbs)** ⭐ **完整示例**  
  领域驱动设计（DDD）、Repository、状态机、批量处理

### 金融策略示例

- **[动量策略](../playbook/ashare_momentum_shadow.tbs)**  
  技术指标、风险调整动量

- **[Alpha 因子选择](../playbook/alpha_factor_selection.tbs)**  
  多因子模型、微观结构、行为偏差

- **[小波去噪](../playbook/wavelet_hht_denoising.tbs)**  
  信号处理、HHT、EMD

---

## 🎯 快速导航

### 按角色导航

| 角色 | 推荐阅读顺序 |
|------|-------------|
| **新手用户** | [DataBind 完整指南](./DATA_BIND_GUIDE.md) → [类包装示例](../examples/test_data_bind_class_wrapping.tbs) → [最佳实践](./data_bind_best_practices.md) |
| **应用开发者** | [语言指南](./language-guide.md) → [交易系统示例](../examples/data_bind_trading_system.tbs) → [API 参考](../turbo_script/include/turbo_script.h) |
| **库集成者** | [DataBind C API](../tbe/data_bind/README.md) → [TurboScript API](../turbo_script/include/turbo_script.h) → [安全说明](../tbe/data_bind/README.md#security-considerations) |
| **量化策略师** | [Alpha 因子示例](../playbook/alpha_factor_selection.tbs) → [动量策略](../playbook/ashare_momentum_shadow.tbs) → [小波去噪](../playbook/wavelet_hht_denoising.tbs) |

### 按任务导航

| 任务 | 相关文档 |
|------|---------|
| **解析 JSON/CSV/XML** | [快速开始](./DATA_BIND_GUIDE.md#🚀-快速开始) → [支持的格式](./DATA_BIND_GUIDE.md#q6-支持哪些数据格式) |
| **添加业务逻辑** | [Plain Object vs Class](./DATA_BIND_GUIDE.md#🎯-核心概念) → [类包装示例](../examples/test_data_bind_class_wrapping.tbs) |
| **性能优化** | [优化策略](./DATA_BIND_GUIDE.md#q4-如何优化性能) → [最佳实践](./data_bind_best_practices.md#性能优化) |
| **错误处理** | [错误处理指南](./DATA_BIND_GUIDE.md#q5-如何处理错误) → [错误码映射](../tbe/data_bind/README.md#error-code-mapping) |
| **架构设计** | [分层架构](./DATA_BIND_GUIDE.md#q7-如何设计架构) → [Repository 模式](./data_bind_best_practices.md#1-repository-模式) |

---

## 🔧 模块文档

### 核心模块

- **[exprtk](../exprtk/)** - 表达式引擎、解释器
- **[turbo_script](../turbo_script/)** - 脚本引擎、JIT 编译
- **[tbe](../tbe/)** - TurboScript Binary Encoding、DataBind
- **[modules](../modules/)** - 内置模块（json、csv、xml、img 等）

### 测试与基准

- **[exprtk 测试](../exprtk/test/)** - 表达式引擎单元测试
- **[turbo_script 测试](../turbo_script/test/)** - JIT 编译器测试
- **[data_bind 测试](../tbe/data_bind/)** - DataBind 功能测试

---

## 📖 常见场景

### 场景 1：解析 API 响应

```javascript
import("data_bind");

var codec = data_bind.create("api_schema.tbe");
var response = api_get("/users/123");
var user = data_bind.parse_json(codec, "User", response);

print(user.name);
print(user.email);
```

**相关文档**：
- [快速开始](./DATA_BIND_GUIDE.md#🚀-快速开始)
- [JSON 解析](../tbe/data_bind/README.md#supported-inputs)

---

### 场景 2：批量数据处理

```javascript
var codec = data_bind.create("log_schema.tbe");
var logs = data_bind.parse_json_all(codec, "LogEvent", json_array);

var error_count = 0;
for (var i = 0; i < len(logs); i++) {
    if (logs[i].level == "ERROR") error_count += 1;
}
```

**相关文档**：
- [批量处理](./data_bind_best_practices.md#5-批量处理优化)
- [性能优化](./DATA_BIND_GUIDE.md#q4-如何优化性能)

---

### 场景 3：领域模型设计

```javascript
class Order {
    static from_json(codec, json) {
        var plain = data_bind.parse_json(codec, "Order", json);
        return Order.from_plain(plain);
    }
    
    can_cancel() {
        return this.status == "PENDING";
    }
    
    cancel() {
        if (!this.can_cancel()) {
            throw "Cannot cancel order in status: " + this.status;
        }
        this.status = "CANCELLED";
    }
}
```

**相关文档**：
- [类包装模式](../examples/test_data_bind_class_wrapping.tbs)
- [交易系统示例](../examples/data_bind_trading_system.tbs)
- [架构模式](./data_bind_best_practices.md#架构模式)

---

### 场景 4：高性能 RPC

```javascript
// 使用 Binary TBE 格式（比 JSON 快 10 倍）
var codec = data_bind.create("rpc_schema.tbe");
var request = data_bind.parse(codec, "Request", binary_payload);

// 处理请求
var response = process_request(request);

// 序列化响应
var response_binary = data_bind.to_binary(codec, response);
socket.send(response_binary);
```

**相关文档**：
- [Binary TBE 格式](../tbe/data_bind/README.md#minimal-use)
- [格式对比](./DATA_BIND_GUIDE.md#q6-支持哪些数据格式)

---

## 🛠️ 开发工具

### 编译与构建

```bash
# 配置项目
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build --config Release

# 运行测试
cd build && ctest -C Release
```

### 运行示例

```bash
# 运行 TurboScript 脚本
./build/bin/turbo_script examples/test_lambda.tbs

# 运行 DataBind 示例
./build/bin/turbo_script examples/test_data_bind_class_wrapping.tbs
```

---

## 🤝 贡献指南

### 文档贡献

1. **修正错误**：发现文档错误或过时内容？提交 Pull Request
2. **添加示例**：有实用的代码片段？添加到 `examples/` 目录
3. **完善指南**：有最佳实践经验？扩充 `docs/data_bind_best_practices.md`

### 代码贡献

参考项目根目录的 [AGENTS.md](../AGENTS.md)，了解：
- 代码规范
- 提交流程
- 测试要求

---

## 📞 获取帮助

### 常见问题

查看 [DataBind 完整指南 - 常见问题](./DATA_BIND_GUIDE.md#📖-常见问题)

### 问题反馈

1. **Bug 报告**：提交 GitHub Issue
2. **功能请求**：提交 GitHub Issue 并标记 `enhancement`
3. **文档问题**：提交 Pull Request 或 Issue

---

## 📝 更新日志

### 最近更新

- **2026-07-02**: 新增 DataBind 完整指南、最佳实践文档、类包装示例、交易系统示例
- **2026-07-02**: 优化 DataBind 错误处理、统一错误路径格式
- **2026-07-02**: 实现 MIR 模块缓存（100x 性能提升）
- **2026-07-02**: 实现值对象池（30-50% 性能提升）
- **2026-07-02**: 添加 Schema 验证（防止偏移溢出、嵌套过深、循环引用）

---

## 📄 许可证

本文档与 TurboScript 项目使用相同许可证。
