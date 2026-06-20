# TurboScript 语言指南

TurboScript 语法和语言特性完整参考。

---

## 目录

1. [数据类型](#数据类型)
2. [变量和常量](#变量和常量)
3. [运算符](#运算符)
4. [控制流](#控制流)
5. [函数](#函数)
6. [集合类型](#集合类型)
7. [面向对象编程](#面向对象编程)
8. [模块和导入](#模块和导入)
9. [高级特性](#高级特性)
10. [错误处理](#错误处理)
11. [安全限制](#安全限制)

---

## 数据类型

TurboScript 支持六种核心数据类型：

### 数字（Number）

64 位浮点数：

```javascript
var integer = 42;
var decimal = 3.14159;
var scientific = 1.5e10;
var negative = -273.15;
```

### 字符串（String）

UTF-8 编码字符串，支持单引号或双引号：

```javascript
var greeting = "Hello, World!";
var message = '单引号也可以';
var template = `模板字符串`;
```

### 向量（Vector）

同构数字数组：

```javascript
var prices = [100, 102, 104, 103, 105];
var empty = [];
var range = [1, 2, 3, 4, 5];
```

### 映射（Map）

基于哈希表的键值对：

```javascript
var person = map{
    name: "Alice",
    age: 30,
    city: "NYC"
};

var nested = map{
    user: map{name: "Bob"},
    scores: [95, 87, 92]
};
```

### 列表（List）

异构集合（混合类型）：

```javascript
var mixed = list("hello", 42, [1, 2, 3], map{key: "value"});
var item = mixed[0];  // "hello"
```

### 空值（Null）

表示值的缺失：

```javascript
var empty = null;
var nothing = nil;  // 等价于 null
```

### 内置常量

| 常量 | 值 | 说明 |
|----------|-------|-------------|
| `pi` | 3.14159265358979… | 圆周率 π |
| `e` | 2.71828182845904… | 自然常数 |
| `inf` | ∞ | 正无穷 |
| `nan` | NaN | 非数字 |
| `true` | 1.0 | 布尔真 |
| `false` | 0.0 | 布尔假 |
| `null` / `nil` | null | 空值 |

### 类型内省

```javascript
typeof(42)          // "number"
typeof("hello")     // "string"
typeof([1,2,3])     // "vector"
typeof(map{a: 1})   // "map"
typeof(null)        // "null"

is_number(42)       // 1 (true)
is_string("hi")     // 1 (true)
is_vector([1,2])    // 1 (true)
is_map(map{})       // 1 (true)
is_null(null)       // 1 (true)
```

---

## 变量和常量

### 变量声明

```javascript
// 使用 var 关键字
var x = 10;
var name = "Alice";

// 使用 let 关键字（等价于 var）
let y = 20;
let message = "Hello";

// 不使用关键字（隐式声明）
z = 30;
```

### 常量

```javascript
const PI = 3.14159;
const MAX_SIZE = 1000;

// PI = 3.14;  // 错误：无法重新赋值常量
```

### 赋值运算符

```javascript
x = 10;      // 简单赋值
x += 5;      // x = x + 5  (15)
x -= 3;      // x = x - 3  (12)
x *= 2;      // x = x * 2  (24)
x /= 4;      // x = x / 4  (6)
```

### 解构赋值

从向量和映射中提取值：

```javascript
// 向量解构
var [a, b, c] = [10, 20, 30];
// a = 10, b = 20, c = 30

// 跳过元素
var [first, , third] = [1, 2, 3];
// first = 1, third = 3

// 剩余参数
var [head, ...tail] = [1, 2, 3, 4, 5];
// head = 1, tail = [2, 3, 4, 5]

// 映射解构
var map{name, age} = map{name: "Alice", age: 30};
// name = "Alice", age = 30

// 嵌套解构
var [[x, y], z] = [[1, 2], 3];
// x = 1, y = 2, z = 3
```

---

## 运算符

### 算术运算符

```javascript
x + y    // 加法
x - y    // 减法
x * y    // 乘法
x / y    // 除法
x % y    // 取模
x ^ y    // 幂运算
```

### 比较运算符

```javascript
x == y   // 等于
x != y   // 不等于
x <> y   // 不等于（替代写法）
x < y    // 小于
x <= y   // 小于等于
x > y    // 大于
x >= y   // 大于等于
```

### 逻辑运算符

```javascript
// 单词形式
x and y  // 逻辑与
x or y   // 逻辑或
not x    // 逻辑非

// 符号形式（等价）
x && y   // 逻辑与
x || y   // 逻辑或
!x       // 逻辑非
```

### 特殊运算符

#### 三元运算符

```javascript
var result = condition ? value_if_true : value_if_false;

var status = age >= 18 ? "成年人" : "未成年";
```

#### 管道操作符

将值作为第一个参数传递给下一个函数：

```javascript
// 传统嵌套
var result = sum(filter(map(data, f), g));

// 使用管道操作符
var result = data |> map(f) |> filter(g) |> sum();

// 实际示例
prices |> ta.sma(20) |> ta.rsi(14);
// 等价于：ta.rsi(ta.sma(prices, 20), 14)
```

#### 可选链

安全的属性访问，返回 null 而不是错误：

```javascript
var user = map{
    name: "Alice",
    address: map{city: "NYC"}
};

var city = user?.address?.city;  // "NYC"
var zip = user?.address?.zip;    // null（无错误）

var missing = null;
var value = missing?.property;   // null（无错误）
```

#### 展开运算符

在函数调用或数组字面量中展开元素：

```javascript
var arr = [2, 3, 4];
sum(1, ...arr, 5);  // sum(1, 2, 3, 4, 5)

var combined = [1, ...arr, 5];  // [1, 2, 3, 4, 5]
```

---

## 控制流

### If 语句

```javascript
if (condition) {
    // 代码
}

if (condition) {
    // 代码
} else {
    // 代码
}

if (condition1) {
    // 代码
} elif (condition2) {
    // 代码
} else {
    // 代码
}
```

### While 循环

```javascript
var i = 0;
while (i < 10) {
    print(i);
    i += 1;
}
```

### Do-While 循环

```javascript
var i = 0;
do {
    print(i);
    i += 1;
} while (i < 10);
```

### For 循环

```javascript
// 传统 for 循环
for (var i = 0; i < 10; i += 1) {
    print(i);
}

// For-in 循环（遍历向量）
for (item in [10, 20, 30]) {
    print(item);
}
```

### Switch 语句

```javascript
switch (status) {
    case 1:
        print("活跃");
    case 2:
        print("待处理");
    case 3:
        print("非活跃");
    default:
        print("未知");
}
```

### Break 和 Continue

```javascript
for (var i = 0; i < 10; i += 1) {
    if (i == 5) continue;  // 跳过 5
    if (i == 8) break;     // 在 8 处停止
    print(i);
}
```

---

## 函数

### 函数定义

```javascript
func add(a, b) {
    return a + b;
}

var result = add(5, 3);  // 8
```

### 简写函数定义

```javascript
// 使用赋值语法
square(x) = x * x;
area(w, h) = w * h;

var result = square(5);  // 25
```

### 匿名函数

```javascript
var multiply = func(a, b) {
    return a * b;
};

var result = multiply(4, 5);  // 20
```

### 箭头函数（Lambda）

```javascript
// 单表达式（隐式返回）
var double = (x) => x * 2;
var add = (a, b) => a + b;

// 无参数
var getPI = () => 3.14159;

// 块体（显式返回）
var complex = (x) => {
    var temp = x * 2;
    return temp + 1;
};

// 使用
var result = double(5);  // 10
```

### 默认参数

```javascript
func greet(name = "World", greeting = "Hello") {
    return greeting + ", " + name + "!";
}

greet();                    // "Hello, World!"
greet("Alice");             // "Hello, Alice!"
greet("Bob", "Hi");         // "Hi, Bob!"
```

### 可变参数函数

函数可以接受可变数量的参数：

```javascript
func sum_all(...args) {
    var total = 0;
    for (arg in args) {
        total += arg;
    }
    return total;
}

sum_all(1, 2, 3);        // 6
sum_all(1, 2, 3, 4, 5);  // 15
```

### 递归

```javascript
func factorial(n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

factorial(5);  // 120

func fibonacci(n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

fibonacci(10);  // 55
```

### 闭包

函数可以捕获外部作用域的变量：

```javascript
func makeCounter() {
    var count = 0;
    return func() {
        count += 1;
        return count;
    };
}

var counter = makeCounter();
counter();  // 1
counter();  // 2
counter();  // 3
```

---

## 集合类型

### 向量操作

```javascript
var v = [10, 20, 30, 40, 50];

// 索引（从 0 开始）
v[0]       // 10
v[4]       // 50

// 切片（不包含结束位置）
v[1..4]    // [20, 30, 40]
v[0..2]    // [10, 20]

// 长度
len(v)     // 5
v.length() // 5（点号风格）

// 聚合
sum(v)     // 150
avg(v)     // 30
min(v)     // 10
max(v)     // 50

// 点号风格（等价）
v.sum()    // 150
v.avg()    // 30
v.min()    // 10
v.max()    // 50

// 修改操作
v.push(60);        // v = [10, 20, 30, 40, 50, 60]
var last = v.pop(); // last = 60, v = [10, 20, 30, 40, 50]

// 排序
var sorted = sort(v);  // 返回排序副本
v.sort();              // 返回排序副本

// 搜索
v.indexOf(30);         // 2
vector_find_value(v, 30);     // 2（第一次出现）
vector_find_all(v, 30);       // [2]（所有出现位置）

// 反转
v.reverse();           // 返回反转副本
```

### 向量算术

元素级操作，支持标量广播：

```javascript
[1, 2, 3] + [4, 5, 6]  // [5, 7, 9]
[1, 2, 3] * 10          // [10, 20, 30]
[10, 20] - [3, 4]       // [7, 16]
[2, 4, 6] / 2           // [1, 2, 3]
```

### 字符串操作

```javascript
var s = "Hello World";

// 大小写转换
s.lower()              // "hello world"
s.upper()              // "HELLO WORLD"

// 修剪
"  hello  ".trim()     // "hello"
"  hello".ltrim()      // "hello"
"hello  ".rtrim()      // "hello"

// 搜索
s.contains("World")    // 1 (true)
s.indexOf("World")     // 6
s.starts_with("Hello") // 1 (true)
s.ends_with("World")   // 1 (true)

// 子串
s.substr(0, 5)         // "Hello"
s[0..5]                // "Hello"（切片）

// 替换
s.replace("World", "TurboScript")  // "Hello TurboScript"

// 长度
s.length()             // 11
len(s)                 // 11

// 反转
s.reverse()            // "dlroW olleH"

// 分割
"a,b,c".split(",")     // ["a", "b", "c"]（返回向量）
```

### 映射操作

```javascript
var m = map{name: "Alice", age: 30, city: "NYC"};

// 访问
m.name                 // "Alice"
m["age"]               // 30

// 检查存在
m.has("city")          // 1 (true)
m.has("country")       // 0 (false)

// 获取键
m.keys()               // ["name", "age", "city"]

// 大小
m.size()               // 3

// 删除
m.delete("city");      // 移除 "city" 键
```

### 列表操作

```javascript
var lst = list("hello", 42, [1, 2, 3]);

// 索引
lst[0]                 // "hello"
lst[1]                 // 42
lst[2]                 // [1, 2, 3]

// 长度
len(lst)               // 3
lst.length()           // 3
```

---

## 面向对象编程

TurboScript 支持基于类的 OOP。`class` 和 `interface` 是语句级声明，不使用 `var` / `let` 前缀。

```javascript
class Counter {
    private value = 0;
    protected static seed = 10;

    constructor(start) {
        this.value = start;
    }

    add(delta) {
        this.value = this.value + delta;
        return this.value;
    }

    static base() {
        return Counter.seed;
    }
}

var c = Counter(40);
var result = c.add(2);  // 42
```

类是运行时值。`Alias = Counter` 会让 `Alias` 引用同一个类对象，因此静态字段共享，`Alias(...)`、`new Alias(...)` 和 `instanceof Alias` 都按别名指向的类值执行。

继承使用 `extends`、`super(...)` 和 `super.method(...)`：

```javascript
class Base {
    constructor(value) { this.value = value; }
    get() { return this.value; }
}

class Child extends Base {
    constructor(value) { super(value + 1); }
    get() { return super.get() + 1; }
}
```

类体可以声明实例字段和静态字段。未写初始化表达式的字段初始值为 `null`。访问控制默认为 `public`；`protected` 允许声明类和子类访问；`private` 只允许声明类访问。

```javascript
class Box {
    value;                 // public，初始值 null
    private secret = 40;
    protected static seed = 2;

    get() {
        return this.secret + Box.seed;
    }
}
```

接口支持 `implements` 和接口继承：

```javascript
interface Shape {
    area();
}

interface NamedShape extends Shape {
    label();
}

class Square implements NamedShape {
    size = 0;

    constructor(size) { this.size = size; }
    area() { return this.size * this.size; }
    label() { return "square"; }
}
```

接口也可以声明静态方法契约。实现类可以直接提供该静态方法，也可以从父类继承。

方法支持按参数个数重载，也支持按声明的类/接口参数类型重载：

```javascript
class Printer {
    print() { return 1; }
    print(x) { return x + 1; }
    print(x: Shape) { return x.area(); }
}
```

当前 OOP 约束：

- 不支持属性风格 getter/setter；使用普通 `get()` / `set(value)` 方法。
- `_name` 私有约定仍用于动态字段和动态方法，但新代码优先使用显式 `private` / `protected`。
- JIT/MIR 通过 runtime helper lowering 执行 OOP 操作；不支持的形式会报错，不会静默回退到解释器。

---

## 模块和导入

### 加载模块

```javascript
// 加载插件模块
import("csv");
import("json");
import("ta");
import("vec");
import("net");
import("sqlite");
import("fin");
import("data_bind");
```

### 模块命名空间

通过模块前缀访问函数：

```javascript
import("csv");

var data = csv.read("data.csv");
var column = csv.col(data, "price");
var filtered = csv.filter(data, "price > 100");
```

### 内置函数（无需导入）

这些函数全局可用：

```javascript
// 数学
sin(x), cos(x), sqrt(x), abs(x), log(x)

// 字符串
lower(s), upper(s), trim(s), substr(s, start, len)

// 向量
sum(v), avg(v), min(v), max(v), len(v)

// 文件 I/O
read_file(path), write_file(path, content), copy_file(src, dst)
file_exists(path), file_size(path)
file_truncate(path, length)
listdir(path), glob(pattern), mkdir_recursive(path), rmdir_recursive(path)

// 日期/时间
now(), date(str), date_utc(str)
format_date(timestamp), format_date_utc(timestamp)

// 平台
os_name(), pid(), uptime_ms(), monotonic_ms()
```

文件与路径内建函数基于 TurboNet `turbo_fs`。日期格式化、本地时间与 UTC
转换基于 TurboNet platform datetime helpers；可选 `parser` 模块还提供结构化
`datetime.parse()` 与 RFC822 格式化辅助函数。

### 导入脚本

加载其他 TurboScript 文件：

```javascript
// utils.ts
func helper(x) {
    return x * 2;
}

// main.ts
import("utils.ts");
var result = helper(5);  // 10
```

---

## 高级特性

### 成员调用分发

点号风格的方法调用在内部被转换：

```javascript
// 用户编写：
prices.sma(20)

// 内部转换为：
sma(prices, 20)
```

这适用于：
- **向量**：`v.sum()`、`v.avg()`、`v.push(x)`
- **字符串**：`s.upper()`、`s.trim()`、`s.substr(0, 5)`
- **映射**：`m.keys()`、`m.has(key)`、`m.delete(key)`
- **模块函数**：`prices.sma(20)` → `ta.sma(prices, 20)`

### 模板字符串

```javascript
var name = "Alice";
var age = 30;
var message = `你好，${name}！你今年 ${age} 岁。`;
```

### Mustache 模板渲染

当模板文本需要由 map/list/vector 数据驱动，而不是直接做表达式插值时，使用
`template_render(template, data [, partials])`。`mustache_render` 是它的别名。

```javascript
var data = map{
    name: "Ada",
    html: "<b>",
    items: list(map{name: "one"}, map{name: "two"})
};

var out = template_render(
    "Hi {{name}} {{html}} {{{html}}} {{#items}}{{name}};{{/items}}",
    data
);
// "Hi Ada &lt;b&gt; <b> one;two;"
```

map 支持点路径查找（`{{user.city}}`），list 和 vector 支持 section，缺失值输出为空。
第三个参数 `partials` 是 partial 模板 map，用于 `{{>name}}`，也支持
`{{>layout.item}}` 这样的点路径 partial 名。数据 map 中的函数值会作为 Mustache
lambda：接收原始 section 内容，返回的模板文本会在当前上下文中继续渲染。

---

## 错误处理

### Try-Catch

```javascript
try {
    var result = risky_operation();
    if (result < 0) {
        throw "不允许负数结果";
    }
} catch (error) {
    print("错误: " + error);
}
```

### 断言

```javascript
func divide(a, b) {
    assert(b != 0, "除数不能为零");
    return a / b;
}

divide(10, 0);  // 中止并显示消息："除数不能为零"
```

---

## 安全限制

TurboScript 强制执行资源限制以防止失控脚本：

### 递归限制

```javascript
// 默认：1000 层
func infinite() {
    return infinite();  // 1000 次调用后出错
}
```

### 循环迭代限制

```javascript
// 默认：1,000,000 次迭代
var i = 0;
while (true) {
    i += 1;  // 1,000,000 次迭代后出错
}
```

### 配置限制（C API）

```c
turbo_script_ctx_t *ctx = turbo_script_init();
ctx->env->max_recursion = 500;
ctx->env->max_loop_iterations = 100000;
```

---

## 注释

```javascript
// 单行注释

/*
 * 多行注释
 * 可以跨越多行
 */

var x = 10;  // 行内注释
```

---

## 分号

分号在大多数情况下是**可选的**：

```javascript
// 带分号
var x = 10;
var y = 20;

// 不带分号（也有效）
var x = 10
var y = 20

// 块后面的分号是可选的
func test() {
    return 42;
}  // 不需要分号
```

---

## 最佳实践

### 1. 使用描述性名称

```javascript
// 好
var closing_prices = [100, 102, 104];
var moving_average = ta.sma(closing_prices, 20);

// 差
var x = [100, 102, 104];
var y = ta.sma(x, 20);
```

### 2. 优先使用点号风格以提高可读性

```javascript
// 好
var total = prices.sum();
var average = prices.avg();

// 也可以
var total = sum(prices);
var average = avg(prices);
```

### 3. 使用管道操作符进行链式操作

```javascript
// 好
var result = data
    |> filter(x > 0)
    |> map(x => x * 2)
    |> sum();

// 难以阅读
var result = sum(map(filter(data, x > 0), x => x * 2));
```

### 4. 显式处理错误

```javascript
// 好
try {
    var data = read_file("data.txt");
    process(data);
} catch (e) {
    print("读取文件失败: " + e);
}

// 有风险
var data = read_file("data.txt");  // 可能静默失败
```

---

## 下一步

- **[API 参考](api-reference.md)** - 完整函数参考
- **[插件开发](plugin-development.md)** - 使用 C/C++ 扩展 TurboScript
- **[模块文档](../modules/)** - 领域特定指南

---

**祝你编程愉快！**
