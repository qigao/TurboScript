# DataBind 完整指南

本文档汇总 TurboScript DataBind 模块的所有使用指南、最佳实践和示例代码。

## 📚 文档目录

### 1. 基础文档

- **[DataBind C API 文档](../tbe/data_bind/README.md)**  
  完整的 C API 参考、错误处理、安全说明、性能优化配置

- **[TurboScript 语言指南](./language-guide.md)**  
  TurboScript 脚本语言语法、类型系统、OOP 特性

### 2. 最佳实践

- **[DataBind 最佳实践指南](./data_bind_best_practices.md)**  
  性能优化、架构模式、错误处理、常见陷阱

### 3. 示例代码

- **[类包装模式示例](../examples/test_data_bind_class_wrapping.tbs)**  
  三种将 Plain Object 包装为 Class Instance 的模式

- **[交易系统示例](../examples/data_bind_trading_system.tbs)**  
  完整的领域驱动设计（DDD）示例，包含 Repository、状态机、批量处理

---

## 🚀 快速开始

### 步骤 1：创建 Schema

```tbe
message User {
    string name;
    int age;
    string email;
}
```

### 步骤 2：解析数据

```javascript
import("data_bind");

// 创建 codec
var codec = data_bind.create("user.tbe");

// 解析 JSON
var json = '{"name":"Alice","age":30,"email":"alice@example.com"}';
var user = data_bind.parse_json(codec, "User", json);

// 访问字段
print(user.name);  // "Alice"
print(user.age);   // 30
```

### 步骤 3：添加业务逻辑（可选）

```javascript
class User {
    static from_json(codec, json_text) {
        var plain = data_bind.parse_json(codec, "User", json_text);
        var user = new User();
        user.name = plain.name;
        user.age = plain.age;
        user.email = plain.email;
        return user;
    }
    
    greet() {
        return "Hello, " + this.name;
    }
    
    is_adult() {
        return this.age >= 18;
    }
}

var user = User.from_json(codec, json);
print(user.greet());  // "Hello, Alice"
```

---

## 🎯 核心概念

### Plain Object vs Class Instance

| 特性 | Plain Object | Class Instance |
|------|--------------|----------------|
| **创建方式** | `data_bind.parse_*()` | `new Class()` |
| **字段访问** | ✅ `obj.field` | ✅ `obj.field` |
| **方法调用** | ❌ 不支持 | ✅ `obj.method()` |
| **性能** | 🚀 最快 | 🚀 快（有包装开销） |
| **适用场景** | 数据处理 | 业务逻辑 |

**关键设计原则**：
- Plain Object 用于**数据传输和处理**（ETL、序列化、API）
- Class Instance 用于**业务逻辑和行为**（验证、状态机、工作流）

---

## 📖 常见问题

### Q1: DataBind 返回的是什么类型？

**A**: DataBind 返回 **Plain Object**（`EXPRTK_VAL_OBJECT`），它是纯数据结构，类似于：
- JavaScript 的 `Object`
- Python 的 `dict`
- Java 的 `Map<String, Object>`

Plain Object **没有方法**，只有字段。

---

### Q2: 如何给 Plain Object 添加方法？

**A**: 包装成 Class Instance，有三种模式：

**模式 A: 构造函数包装**（适合小型项目）
```javascript
class User {
    constructor(plain) {
        this.name = plain.name;
        this.age = plain.age;
    }
    greet() { return "Hello, " + this.name; }
}
var user = new User(plain);
```

**模式 B: 工厂函数**（适合解耦测试）
```javascript
function create_user(plain) {
    var user = new User();
    user.name = plain.name;
    user.age = plain.age;
    return user;
}
var user = create_user(plain);
```

**模式 C: 静态工厂方法**（推荐，API 清晰）
```javascript
class User {
    static from_json(codec, json) {
        var plain = data_bind.parse_json(codec, "User", json);
        var user = new User();
        user.name = plain.name;
        user.age = plain.age;
        return user;
    }
    greet() { return "Hello, " + this.name; }
}
var user = User.from_json(codec, json);
```

详见：[类包装模式示例](../examples/test_data_bind_class_wrapping.tbs)

---

### Q3: Plain Object 和 Class Instance 性能差距多大？

**A**: 取决于使用场景：

| 场景 | Plain Object | Class Instance | 差距 |
|------|--------------|----------------|------|
| 纯字段访问 | 100ms | 110ms | ~10% |
| 频繁方法调用 | N/A | 150ms | N/A |
| 批量解析（1000条） | 500ms | 700ms | ~40% |

**建议**：
- 数据密集型任务：使用 Plain Object
- 业务逻辑密集型：使用 Class Instance
- 混合场景：延迟包装（只在需要方法时包装）

---

### Q4: 如何优化性能？

**A**: 五大优化策略：

1. **复用 Codec**（100x 提升）
   ```javascript
   var codec = data_bind.create("schema.tbe");  // 只创建一次
   for (var i = 0; i < 10000; i++) {
       var obj = data_bind.parse_json(codec, "Data", json_array[i]);
   }
   ```

2. **使用 Binary 格式**（10x 提升 vs JSON）
   ```javascript
   var obj = data_bind.parse(codec, "Data", binary_data);  // 最快
   ```

3. **启用缓存**（自动，30-50% 提升）
   ```javascript
   // 默认已启用，无需手动配置
   ```

4. **批量处理**（20-40% 提升）
   ```javascript
   var objects = data_bind.parse_json_all(codec, "Data", json_array);
   ```

5. **避免过度包装**（20-40% 提升）
   ```javascript
   // ❌ 不必要的包装
   var obj = Class.from_plain(plain);
   var value = obj.field;  // 只访问字段
   
   // ✅ 直接使用 Plain Object
   var value = plain.field;
   ```

详见：[最佳实践指南](./data_bind_best_practices.md)

---

### Q5: 如何处理错误？

**A**: 使用严格验证 + 错误路径跟踪：

```javascript
try {
    // 严格验证（生产环境推荐）
    data_bind.validate_json(codec, "User", json);
    var user = data_bind.parse_json(codec, "User", json);
} catch (err) {
    // 错误路径指示具体位置
    print("Error at: " + err.path);        // "json: $.address.zipcode"
    print("Message: " + err.message);      // "Expected string, got number"
    print("Line: " + err.line);            // 15
    print("Column: " + err.column);        // 12
}
```

---

### Q6: 支持哪些数据格式？

| 格式 | 解析 API | 生成 API | 性能 | 适用场景 |
|------|---------|---------|------|---------|
| **Binary TBE** | `parse()` | `to_binary()` | 🚀🚀🚀 最快 | RPC、内部通信 |
| **JSON** | `parse_json()` | `to_json()` | 🚀 快 | API、配置 |
| **CSV** | `parse_csv()` | `to_csv()` | 🚀🚀 很快 | 报表、导出 |
| **XML** | `parse_xml()` | `to_xml()` | 🐢 慢 | 遗留系统 |

---

### Q7: 如何设计架构？

**A**: 推荐分层架构：

```
┌─────────────────────────────────────┐
│  表现层 (UI / API)                   │
├─────────────────────────────────────┤
│  业务逻辑层 (Class Instance)         │
│  - 验证、状态机、工作流              │
├─────────────────────────────────────┤
│  数据访问层 (Repository)             │
│  - Plain Object ↔ Class 转换        │
├─────────────────────────────────────┤
│  数据传输层 (Plain Object)           │
│  - DataBind 解析/序列化              │
├─────────────────────────────────────┤
│  数据源 (JSON / Binary / CSV / XML)  │
└─────────────────────────────────────┘
```

示例代码：[交易系统示例](../examples/data_bind_trading_system.tbs)

---

### Q8: TurboScript 是否支持完整的 OOP？

**A**: ✅ **完全支持！** TurboScript 是完整的面向对象语言：

```javascript
// 类定义
class Animal {
    name = "";
    
    constructor(name) {
        this.name = name;
    }
    
    speak() {
        return "...";
    }
}

// 继承
class Dog extends Animal {
    breed = "";
    
    constructor(name, breed) {
        super(name);
        this.breed = breed;
    }
    
    speak() {
        return "Woof! I'm " + this.name;
    }
}

// 多态
var dog = new Dog("Buddy", "Golden Retriever");
print(dog.speak());  // "Woof! I'm Buddy"
```

**DataBind 的限制**只在于它返回 **Plain Object**（纯数据），而不是 **Class Instance**（数据+行为）。这是设计选择，不是语言限制。

---

## 🛠️ 工具和调试

### 查看 Schema 反射

```javascript
// 查询类型信息
var types = data_bind.schema_types(codec);
for (var i = 0; i < len(types); i++) {
    print("Type: " + types[i].name);
    print("  Fields: " + types[i].field_count);
}

// 查询字段信息
var fields = data_bind.schema_fields(codec, "User");
for (var i = 0; i < len(fields); i++) {
    print("Field: " + fields[i].name + " : " + fields[i].type);
}
```

### 性能统计

```javascript
// 启用统计
data_bind.enable_jit_stats(true);

// ... 执行解析操作 ...

// 查看统计
var stats = data_bind.get_jit_stats();
print("Compile count: " + stats.compile_count);
print("Cache hit rate: " + (stats.cache_hit / (stats.cache_hit + stats.cache_miss) * 100) + "%");
```

### 对象池统计

```javascript
var stats = data_bind.get_value_pool_stats();
print("Pool efficiency: " + (stats.reused / stats.allocated * 100) + "%");
```

---

## 📦 相关资源

### 官方文档
- [DataBind C API](../tbe/data_bind/README.md)
- [TurboScript 语言指南](./language-guide.md)

### 示例代码
- [基础示例：Lambda 和闭包](../examples/test_lambda.tbs)
- [IO 函数示例](../examples/test_io_functions.tbs)
- [类包装模式](../examples/test_data_bind_class_wrapping.tbs)
- [完整交易系统](../examples/data_bind_trading_system.tbs)

### 最佳实践
- [性能优化](./data_bind_best_practices.md#性能优化)
- [架构模式](./data_bind_best_practices.md#架构模式)
- [错误处理](./data_bind_best_practices.md#错误处理)

---

## 🤝 贡献

发现文档问题或有改进建议？欢迎：
1. 提交 Issue
2. 创建 Pull Request
3. 补充示例代码

---

## 📄 许可证

本文档与 TurboScript 项目使用相同许可证。
