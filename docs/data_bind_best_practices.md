# DataBind 最佳实践指南

本文档总结 TurboScript DataBind 模块的使用最佳实践、性能优化建议和常见模式。

## 目录

1. [Plain Object vs Class Instance](#plain-object-vs-class-instance)
2. [类包装模式选择](#类包装模式选择)
3. [性能优化](#性能优化)
4. [错误处理](#错误处理)
5. [架构模式](#架构模式)
6. [常见陷阱](#常见陷阱)

---

## Plain Object vs Class Instance

### Plain Object 适用场景

✅ **纯数据处理场景**

```javascript
// 金融数据分析：无需方法，只需字段访问
import("data_bind");
import("finance");

var codec = data_bind.create("market_data.tbe");
var tick = data_bind.from_binary(codec, binary_data);

// 直接使用字段进行计算
var ma = finance.sma(tick.close, 20);
var rsi = finance.rsi(tick.close, 14);

if (rsi > 70) {
    print("Overbought: " + tick.symbol);
}
```

✅ **批量 ETL 管道**

```javascript
// 日志处理：高吞吐量，无需行为封装
var codec = data_bind.create("log_event.tbe");
var logs = data_bind.parse_json_all(codec, "LogEvent", json_array);

var error_count = 0;
var warning_count = 0;

for (var i = 0; i < len(logs); i++) {
    var log = logs[i];
    if (log.level == "ERROR") error_count += 1;
    if (log.level == "WARNING") warning_count += 1;
}

print("Errors: " + error_count + ", Warnings: " + warning_count);
```

✅ **中间数据格式**

```javascript
// API 响应解析后立即转换为其他格式
var response = data_bind.parse_json(codec, "ApiResponse", json_text);
var csv_output = data_bind.to_csv(codec, response);
write_file("output.csv", csv_output);
```

---

### Class Instance 适用场景

✅ **业务逻辑封装**

```javascript
// 用户管理：需要验证、权限检查等行为
class User {
    static from_json(codec, json_text) {
        var plain = data_bind.parse_json(codec, "User", json_text);
        return User.from_plain(plain);
    }
    
    static from_plain(plain) {
        var user = new User();
        user.id = plain.id;
        user.email = plain.email;
        user.role = plain.role;
        return user;
    }
    
    has_permission(resource) {
        if (this.role == "admin") return true;
        if (this.role == "user" && resource == "read") return true;
        return false;
    }
    
    validate_email() {
        return this.email.indexOf("@") > 0;
    }
}
```

✅ **状态机和工作流**

```javascript
// 订单状态转换：需要状态验证和副作用
class Order {
    static from_json(codec, json_text) { /* ... */ }
    
    can_cancel() {
        return this.status == "PENDING" || this.status == "CONFIRMED";
    }
    
    cancel() {
        if (!this.can_cancel()) {
            throw "Cannot cancel order in status: " + this.status;
        }
        this.status = "CANCELLED";
        this.cancelled_at = now();
        this.send_cancellation_email();
    }
    
    send_cancellation_email() {
        print("Sending cancellation email for order #" + this.id);
    }
}
```

✅ **领域模型（DDD）**

```javascript
// 交易策略：复杂业务规则
class TradingStrategy {
    static from_json(codec, json_text) { /* ... */ }
    
    should_buy(market_data) {
        var rsi = this.calculate_rsi(market_data.close);
        var ma_cross = this.check_ma_crossover(market_data.close);
        return rsi < this.rsi_threshold && ma_cross == "bullish";
    }
    
    calculate_position_size(account_balance) {
        return account_balance * this.risk_percentage / 100;
    }
    
    calculate_rsi(prices) { /* ... */ }
    check_ma_crossover(prices) { /* ... */ }
}
```

---

## 类包装模式选择

### 决策树

```
需要从 Plain Object 创建 Class Instance？
│
├─ 是 → Schema 类型少（<5个）？
│   │
│   ├─ 是 → 使用 [模式 A: 构造函数包装]
│   │        - 简单直接
│   │        - 易于理解
│   │
│   └─ 否 → Schema 类型多（≥5个）？
│       │
│       ├─ 是 → 需要多种输入格式（JSON/CSV/XML/Binary）？
│       │   │
│       │   ├─ 是 → 使用 [模式 C: 静态工厂方法]
│       │   │        - 统一 API
│       │   │        - 格式抽象
│       │   │
│       │   └─ 否 → 使用 [模式 B: 工厂函数]
│       │            - 解耦类定义
│       │            - 便于测试
│       │
│       └─ 需要共享包装逻辑？
│           └─ 使用 [模式 B: 工厂函数]
│                    - 集中管理
│                    - 代码复用
│
└─ 否 → 直接使用 Plain Object
         - 性能最优
         - 内存最少
```

---

### 模式对比表

| 维度 | 模式 A | 模式 B | 模式 C |
|------|--------|--------|--------|
| **代码量** | 中等 | 少 | 多 |
| **耦合度** | 低（类独立） | 最低（完全解耦） | 高（类依赖 DataBind） |
| **可测试性** | 中 | 高 | 中 |
| **API 清晰度** | 中 | 低 | 高 |
| **多格式支持** | 需重复代码 | 需多个工厂 | 内置支持 |
| **适用类型数** | 1-5 | 5-20 | 任意 |
| **学习曲线** | 低 | 低 | 中 |

---

## 性能优化

### 1. 重用 Codec 实例

❌ **错误示例**：每次解析都创建 codec

```javascript
function process_message(json_text) {
    var codec = data_bind.create("schema.tbe");  // ❌ 每次都重新编译
    var msg = data_bind.parse_json(codec, "Message", json_text);
    // ...
    data_bind.free(codec);
}

for (var i = 0; i < 10000; i++) {
    process_message(messages[i]);  // 10000 次 JIT 编译！
}
```

✅ **正确示例**：复用 codec 实例

```javascript
var codec = data_bind.create("schema.tbe");  // ✅ 只编译一次

function process_message(json_text) {
    return data_bind.parse_json(codec, "Message", json_text);
}

for (var i = 0; i < 10000; i++) {
    var msg = process_message(messages[i]);  // 快 100 倍！
    // ...
}

data_bind.free(codec);
```

**性能提升**：~100x（缓存命中后从 50ms 降到 0.5ms）

---

### 2. 启用自动缓存

✅ **自动缓存**（默认启用）

```javascript
// 缓存自动启用，多个 codec 共享相同 schema 的 JIT 代码
var codec1 = data_bind.create("user.tbe");
var codec2 = data_bind.create("user.tbe");  // 缓存命中，无需重新编译
var codec3 = data_bind.create_from_text(schema_text);  // 如果 schema 相同，也会命中缓存

// 清理不再使用的缓存
data_bind.clear_cache();
```

**适用场景**：
- 多次创建相同 schema 的 codec
- 多线程/多进程环境（每个进程独立缓存）
- 微服务架构（每个请求创建临时 codec）

---

### 3. 使用对象池

✅ **对象池**（默认启用）

```javascript
// 对象池自动管理，无需手动配置
for (var i = 0; i < 10000; i++) {
    var obj = data_bind.parse_json(codec, "Message", json_array[i]);
    process(obj);
    data_bind.value_free(obj);  // 对象回到池中，下次复用
}

// 检查池效率
var stats = data_bind.get_value_pool_stats();
print("Pool efficiency: " + (stats.reused / stats.allocated * 100) + "%");
```

**性能提升**：30-50%（JSON 解析场景）

---

### 4. 选择合适的数据格式

| 格式 | 解析速度 | 文件大小 | 人类可读 | 推荐场景 |
|------|---------|---------|---------|---------|
| **Binary TBE** | 🚀🚀🚀 最快 | ⭐⭐⭐ 最小 | ❌ | 高性能 RPC、内部通信 |
| **JSON** | 🚀 快 | ⭐⭐ 中等 | ✅ | API、配置文件 |
| **CSV** | 🚀🚀 很快 | ⭐⭐ 中等 | ✅ | 表格数据、报表 |
| **XML** | 🐢 慢 | ⭐ 大 | ✅ | 遗留系统、SOAP |

**建议**：
- 内部服务间通信：Binary TBE
- 外部 API：JSON
- 数据导出/报表：CSV
- 避免 XML（除非必须）

---

### 5. 批量处理优化

✅ **使用 `parse_*_all` API**

```javascript
// ❌ 逐条解析：N 次函数调用开销
var results = [];
for (var i = 0; i < json_array.length; i++) {
    var obj = data_bind.parse_json(codec, "Item", json_array[i]);
    results.push(obj);
}

// ✅ 批量解析：1 次函数调用
var results = data_bind.parse_json_all(codec, "Item", json_array_text);
```

**性能提升**：20-40%（取决于数组大小）

---

## 错误处理

### 1. 严格验证 vs 宽松解析

**严格验证**（生产环境推荐）

```javascript
import("data_bind");

var codec = data_bind.create("schema.tbe");

// 严格验证：任何字段错误都会抛出异常
try {
    data_bind.validate_json(codec, "User", json_text);
    var user = data_bind.parse_json(codec, "User", json_text);
    // 此时保证 user 完全符合 schema
} catch (err) {
    print("Validation failed: " + err);
    return null;
}
```

**宽松解析**（数据清洗场景）

```javascript
// 宽松解析：跳过无效记录
var users = data_bind.parse_json_all(codec, "User", json_array_text);
// users 只包含成功解析的记录，无效记录被跳过

print("Parsed " + len(users) + " valid users");
```

---

### 2. 错误路径跟踪

```javascript
try {
    var user = data_bind.parse_json(codec, "User", json_text);
} catch (err) {
    // 错误路径指示具体位置
    // JSON: "json: $.address.zipcode"
    // CSV:  "csv: row 42 col 5"
    // XML:  "xml: /root/user[3]/email"
    print("Error at: " + err.path);
    print("Message: " + err.message);
    print("Line: " + err.line + ", Column: " + err.column);
}
```

---

### 3. 分层错误处理

```javascript
// 数据访问层：转换为领域错误
function load_user(user_id) {
    try {
        var json = api_get("/users/" + user_id);
        var plain = data_bind.parse_json(codec, "User", json);
        return User.from_plain(plain);
    } catch (err) {
        throw "UserNotFound: " + user_id;  // 领域层错误
    }
}

// 业务逻辑层：处理领域错误
try {
    var user = load_user(123);
    user.grant_premium();
} catch (err) {
    if (err.indexOf("UserNotFound") >= 0) {
        print("User does not exist");
    } else {
        throw err;  // 重新抛出未知错误
    }
}
```

---

## 架构模式

### 1. Repository 模式

```javascript
// 数据访问层封装
class UserRepository {
    constructor(codec) {
        this.codec = codec;
    }
    
    find_by_id(id) {
        var json = api_get("/users/" + id);
        var plain = data_bind.parse_json(this.codec, "User", json);
        return User.from_plain(plain);
    }
    
    find_all() {
        var json = api_get("/users");
        var plains = data_bind.parse_json_all(this.codec, "User", json);
        var users = [];
        for (var i = 0; i < len(plains); i++) {
            users.push(User.from_plain(plains[i]));
        }
        return users;
    }
    
    save(user) {
        var plain = user.to_plain();  // 需要在 User 类中实现
        var json = data_bind.to_json(this.codec, plain);
        api_post("/users", json);
    }
}

// 使用
var codec = data_bind.create("user.tbe");
var repo = new UserRepository(codec);

var user = repo.find_by_id(123);
user.email = "newemail@example.com";
repo.save(user);
```

---

### 2. DTO (Data Transfer Object) 模式

```javascript
// Plain Object 作为 DTO
function transfer_user_data(from_service, to_service) {
    // 从服务 A 获取 Plain Object
    var codec_a = data_bind.create("service_a_schema.tbe");
    var dto = data_bind.parse_json(codec_a, "User", from_service.get_user());
    
    // 转换为服务 B 的格式（Plain Object 直接传递）
    var codec_b = data_bind.create("service_b_schema.tbe");
    var json = data_bind.to_json(codec_b, dto);
    to_service.create_user(json);
    
    // 无需创建 Class Instance，提高性能
}
```

---

### 3. Adapter 模式

```javascript
// 适配不同数据源
class DataSourceAdapter {
    constructor(codec, source_type) {
        this.codec = codec;
        this.source_type = source_type;
    }
    
    load(identifier) {
        if (this.source_type == "json") {
            var json = read_file(identifier);
            return data_bind.parse_json(this.codec, "Data", json);
        } else if (this.source_type == "csv") {
            var csv = read_file(identifier);
            return data_bind.parse_csv_all(this.codec, "Data", csv);
        } else if (this.source_type == "binary") {
            var binary = read_binary_file(identifier);
            return data_bind.parse(this.codec, "Data", binary);
        }
        throw "Unknown source type: " + this.source_type;
    }
}

// 使用
var json_adapter = new DataSourceAdapter(codec, "json");
var csv_adapter = new DataSourceAdapter(codec, "csv");

var data1 = json_adapter.load("data.json");
var data2 = csv_adapter.load("data.csv");
// 两者返回相同结构的 Plain Object
```

---

## 常见陷阱

### ❌ 陷阱 1：混淆 Plain Object 和 Class Instance

```javascript
// ❌ 错误：尝试在 Plain Object 上调用方法
var plain = data_bind.parse_json(codec, "User", json);
plain.greet();  // 运行时错误！Plain Object 没有方法

// ✅ 正确：先包装成 Class Instance
var user = User.from_plain(plain);
user.greet();  // OK
```

---

### ❌ 陷阱 2：忘记释放 Codec

```javascript
// ❌ 内存泄漏
function process() {
    var codec = data_bind.create("schema.tbe");
    var obj = data_bind.parse_json(codec, "Data", json);
    // 忘记调用 data_bind.free(codec)
}

// ✅ 正确：总是释放资源
function process() {
    var codec = data_bind.create("schema.tbe");
    try {
        var obj = data_bind.parse_json(codec, "Data", json);
        return obj;
    } finally {
        data_bind.free(codec);
    }
}

// ✅ 更好：复用 codec
var global_codec = data_bind.create("schema.tbe");

function process() {
    return data_bind.parse_json(global_codec, "Data", json);
}
```

---

### ❌ 陷阱 3：过度包装

```javascript
// ❌ 不必要的包装：纯数据处理无需 Class
for (var i = 0; i < 1000000; i++) {
    var plain = data_bind.parse_json(codec, "Tick", json_array[i]);
    var tick = Tick.from_plain(plain);  // 浪费性能
    var price = tick.price;  // 只访问字段，不需要方法
}

// ✅ 直接使用 Plain Object
for (var i = 0; i < 1000000; i++) {
    var tick = data_bind.parse_json(codec, "Tick", json_array[i]);
    var price = tick.price;  // 快得多
}
```

---

### ❌ 陷阱 4：Schema 不匹配

```javascript
// ❌ Schema 定义与实际数据不匹配
var schema = `message User { string name; int age; }`;
var codec = data_bind.create_from_text(schema);

var json = '{"name":"Alice","age":"30"}';  // age 是字符串，不是整数
var user = data_bind.parse_json(codec, "User", json);  // 解析失败

// ✅ 使用严格验证提前发现问题
try {
    data_bind.validate_json(codec, "User", json);
} catch (err) {
    print("Schema validation failed: " + err.message);
    // 修正数据或 schema
}
```

---

## 总结

### 性能优先级

1. **复用 Codec**：100x 性能提升
2. **选择 Binary 格式**：10x 性能提升（vs JSON）
3. **启用缓存和对象池**：30-50% 性能提升
4. **避免过度包装**：20-40% 性能提升
5. **批量处理**：20-40% 性能提升

### 设计原则

1. **数据处理用 Plain Object，业务逻辑用 Class Instance**
2. **优先使用静态工厂方法（模式 C），保持 API 清晰**
3. **Repository 模式隔离数据访问层**
4. **严格验证外部输入，宽松解析内部数据**
5. **总是释放资源，或使用全局 Codec**

---

## 参考资料

- [DataBind C API 文档](../tbe/data_bind/README.md)
- [TurboScript 语言指南](./language-guide.md)
- [示例代码](../examples/test_data_bind_class_wrapping.tbs)
