# TurboScript 语法特性分析

本文从当前 grammar、语言指南和测试覆盖出发，描述 TurboScript 已支持的语法面、语义边界和仍不应宣传为完成的部分。

## 语言定位

TurboScript 已经从表达式求值器演进为一门动态类型脚本语言。它的语法风格接近 JavaScript/C/Go 的混合体：

- C/JavaScript 风格块结构：`{ ... }`
- 动态类型变量：`var` / `let` / 隐式赋值
- 函数声明：`func name(args) { ... }`
- 现代表达式语法：箭头函数、解构、spread、optional chaining、pipe
- class/interface OOP：类、接口、继承、`super`、访问控制、重载

主要适用场景是宿主嵌入式脚本、数据处理、量化金融和插件扩展，而不是独立应用运行时。

## 核心数据类型

| 类型 | 语法示例 | 说明 |
|---|---|---|
| number / integer | `42`, `3.14`, `1e10` | 数值计算主类型，布尔也按 `0/非0` 参与计算 |
| string | `"abc"`, `'abc'`, `` `abc` `` | UTF-8 字符串，支持模板字符串 token |
| vector | `[1, 2, 3]` | 数值数组，适合统计和 TA 运算 |
| list | `list("x", 1, map{})` | 异构集合 |
| map | `map{name: "Alice"}` | 键值结构 |
| null | `null`, `nil` | 空值 |
| class value / instance | `Counter`, `Counter(1)` | OOP 运行时值 |

类型内省函数包括 `typeof`、`is_number`、`is_string`、`is_vector`、`is_map`、`is_list`、`is_null`。

## 变量与赋值

支持三种常见写法：

```javascript
var x = 1;
let y = 2;
z = 3;
```

当前 `var` 与 `let` 等价。`const` 创建只读变量：

```javascript
const PI = 3.14159;
```

赋值能力包括：

- 普通赋值：`x = 1`
- 复合赋值：`x += 1`、`x -= 1`、`x *= 2`、`x /= 2`
- 索引赋值：`v[0] = 10`
- 解构赋值：

```javascript
var [a, b] = [10, 20];
var map{name, age} = map{name: "Alice", age: 30};
```

注意：`class` 和 `interface` 是语句级声明，不使用 `var` / `let` 前缀。

## 表达式与操作符

基础操作符：

- 算术：`+ - * / % ^`
- 比较：`== != <> < <= > >=`
- 逻辑：`and or not`，以及 `&& || !`
- 成员访问：`.`、`?.`
- 索引和切片：`v[i]`、`v[start..end]`
- 三元表达式：`cond ? a : b`
- spread：`...xs`
- pipe：`value |> f(arg)`
- `instanceof`

示例：

```javascript
var label = score > 80 ? "A" : "B";
var city = user?.address?.city;
var out = [1, -2, 3] |> filter(x => x > 0) |> map(x => x * 10);
```

## 控制流

当前语法覆盖了主流脚本控制流：

```javascript
if (x > 0) {
    print("positive");
} elif (x == 0) {
    print("zero");
} else {
    print("negative");
}
```

循环：

```javascript
while (i < 10) { i += 1; }

do {
    i += 1;
} while (i < 10);

for (var i = 0; i < 10; i += 1) {
    if (i == 5) continue;
    if (i == 8) break;
}

for (x in [10, 20, 30]) {
    print(x);
}
```

分支与异常：

```javascript
switch (status) {
    case 1: print("active");
    default: print("unknown");
}

try {
    if (b == 0) throw "division by zero";
} catch (e) {
    print(e);
}
```

## 函数与闭包

命名函数：

```javascript
func add(a, b) {
    return a + b;
}
```

匿名函数和箭头函数：

```javascript
var square = func(x) { return x * x; };
var double = x => x * 2;
var add = (a, b) => a + b;
var make = () => map{id: 1};
```

还支持默认参数、递归和闭包：

```javascript
func greet(name = "world") {
    return "hello " + name;
}

func makeCounter() {
    var count = 0;
    return func() {
        count += 1;
        return count;
    };
}
```

兼容旧式函数定义：

```javascript
f(x) = x * 2;
```

## 面向对象语法

OOP 是当前语法中较完整的一块。类和接口都是运行时值：

```javascript
interface Shape {
    area();
}

class Square implements Shape {
    private size = 0;
    protected static seed = 1;

    constructor(size) {
        this.size = size;
    }

    area() {
        return this.size * this.size;
    }

    static kind() {
        return "square";
    }
}

var s = new Square(4);
var ok = s instanceof Shape;
```

已支持的 OOP 语法：

- `class`
- `interface`
- `abstract class`
- `final class` / `final method` 语法节点
- `extends`
- `implements`
- interface 继承
- constructor
- 实例字段、静态字段
- 实例方法、静态方法
- `this`
- `super(...)`
- `super.method(...)`
- `super.field`
- `new Class(...)` 和 `Class(...)`
- `instanceof ClassOrInterface`
- `public` / `protected` / `private`
- 按参数个数重载
- 按 class/interface 参数类型重载
- class value alias：`Alias = Counter`

当前 OOP 约束：

- 不支持属性式 getter/setter；使用普通 `get()` / `set(value)` 方法。
- `_name` 私有约定仍保留兼容，但新代码应优先使用显式 `private` / `protected`。
- JIT/MIR 通过 runtime helper lowering 执行 OOP；不支持形式报错，不静默回退到解释器。

## 模块与数据绑定

模块通过 `import("name")` 加载，函数以命名空间暴露：

```javascript
import("parser");

var rows = csv.bind_all(schema_text, csv_text, "Order");
var row = json.bind(schema_text, json_text, "Order");
var ok = json.validate(schema_text, json_text, "Order");
```

当前 parser 模块已完成 JSON/CSV schema binding 主线：

- `json.bind` / `json.bind_all` / `json.emit` / `json.validate` / `json.validate_ex`
- `json.parse` / `json.stringify`
- `csv.bind` / `csv.bind_all` / `csv.emit` / `csv.validate` / `csv.validate_ex`
- schema text 与 schema handle 两种入口
- record/composite/group、fixed array、list、set、map
- scalar、enum、flags、union
- optional/default 字段

绑定失败不会制造伪有效值：非法 scalar、非法 record、错误 container shape 会绑定失败；`bind_all` 跳过无法绑定的元素，`validate_ex` 返回诊断。
通用 JSON 映射已支持：JSON object/array 可直接转换为脚本 `map`/`list`，脚本 scalar、`map`、`list`、`vector` 可通过 `json.stringify` 输出 JSON。

## MIR 解释器与 JIT 一致性

TurboScript 当前对外执行模型是“全部 MIR”：同一份 AST 经过验证后 lowering 到 MIR，再选择 MIR 解释器或 MIR JIT 执行。语法分析应区分三层：

- grammar 能解析
- MIR lowering 能覆盖
- MIR 解释器与 MIR JIT 能保持同一语义

当前策略是：MIR lowering 对不支持的形式应报错，不能绕开 MIR pipeline。OOP 相关语法通过 MIR lowering 与 runtime helper 执行，已覆盖主线类、实例、继承、`super`、接口 identity 和方法分派。

## 不应宣称完成的语法

以下能力目前不应当作为已完成语法宣传：

- `async` / `await` 语法糖
- `yield` / generator，grammar 中仍标为暂时禁用
- 属性式 getter/setter
- 包管理器、REPL、调试器、profiler 等工具链能力

## 结论

TurboScript 当前语法可以概括为：

> 动态类型、JS-like、面向数值和数据处理的脚本语言，具备现代表达式语法、函数式管道、结构化控制流、模块系统，以及较完整的 class/interface OOP。

下一阶段语法层面更适合补齐：

- `async` / `await` 语法糖
- 错误栈和调试体验
- grammar、language guide、测试之间的一致性清理
