# TurboScript 快速开始

本指南将帮助你在 5 分钟内编写并运行你的第一个 TurboScript 程序。

TurboScript 是一个 TypeScript/JavaScript-like 语法的嵌入式脚本引擎。脚本会 lowering 到 MIR，同一份 IR 可以通过 MIR 解释器执行，也可以通过 MIR JIT 生成原生代码执行；parser/data-bind、数学、矩阵、时间序列、TA 和金融模块用于宿主应用内的轻量数据分析。

---

## Hello World

创建文件 `hello.tbs`：

```javascript
var name = "World";
print("Hello, " + name + "!");
```

运行：
```bash
turbo_script hello.tbs
```

输出：
```
Hello, World!
```

---

## 基础语法

### 变量和类型

```javascript
// 数字
var age = 25;
var pi = 3.14159;

// 字符串
var greeting = "hello";
var message = 'world';

// 向量（数值数组）
var prices = [100, 102, 104, 103, 105];

// 映射（键值对）
var user = map{name: "Alice", age: 30};

// 空值
var empty = null;  // 或 nil
```

### 运算符

```javascript
// 算术运算
var x = 10 + 5;      // 15
var y = x * 2;       // 30
var z = y / 3;       // 10

// 比较运算
var isEqual = (x == 15);        // 1 (true)
var isGreater = (y > x);        // 1 (true)

// 逻辑运算
var result = (x > 5) and (y < 50);   // 1 (true)
var result2 = (x > 5) && (y < 50);   // 同上
```

### 控制流

```javascript
// If-else
if (age >= 18) {
    print("成年人");
} else {
    print("未成年");
}

// 三元运算符
var status = age >= 18 ? "成年人" : "未成年";

// 循环
for (var i = 0; i < 5; i += 1) {
    print(i);
}

// For-in 循环
for (price in prices) {
    print(price);
}
```

### 函数

```javascript
// 函数定义
func square(x) {
    return x * x;
}

var result = square(5);  // 25

// 箭头函数（Lambda）
var double = (x) => x * 2;
var result2 = double(10);  // 20

// 默认参数
func greet(name = "World") {
    return "Hello, " + name;
}

greet();         // "Hello, World"
greet("Alice");  // "Hello, Alice"
```

---

## 数据操作

### 向量

```javascript
var numbers = [10, 20, 30, 40, 50];

// 索引（从 0 开始）
var first = numbers[0];      // 10
var last = numbers[4];       // 50

// 切片
var middle = numbers[1..4];  // [20, 30, 40]

// 内置操作
var total = sum(numbers);    // 150
var average = avg(numbers);  // 30
var minimum = min(numbers);  // 10
var maximum = max(numbers);  // 50

// 点号风格（等价写法）
var total2 = numbers.sum();     // 150
var average2 = numbers.avg();   // 30
```

### 字符串

```javascript
var text = "Hello World";

// 字符串操作
var lower = text.lower();           // "hello world"
var upper = text.upper();           // "HELLO WORLD"
var length = text.length();         // 11
var contains = text.contains("World");  // 1 (true)

// 子串
var sub = text.substr(0, 5);        // "Hello"

// 字符串切片
var slice = text[0..5];             // "Hello"
```

### 映射

```javascript
var person = map{
    name: "Alice",
    age: 30,
    city: "NYC"
};

// 访问值
var name = person.name;              // "Alice"
var age = person["age"];             // 30

// 检查存在
var hasCity = person.has("city");    // 1 (true)

// 获取键
var keys = person.keys();            // ["name", "age", "city"]
```

---

## 文件操作

TurboScript 内置文件 I/O（无需 import）：

```javascript
// 写入文件
write_file("output.txt", "Hello, File!");

// 读取文件
var content = read_file("output.txt");
print(content);  // "Hello, File!"

// 检查文件是否存在
if (file_exists("data.txt")) {
    var data = read_file("data.txt");
    print(data);
}

// 文件信息
var size = file_size("output.txt");  // 字节数
var isFile = is_file("output.txt");  // 1 (true)

// 复制与截断
copy_file("output.txt", "backup.txt");
file_truncate("backup.txt", 5);

// 目录列举
var files = listdir(".");
var scripts = glob("*.tbs");

// 递归目录
mkdir_recursive("data/tmp/cache");
rmdir_recursive("data/tmp");
```

日期时间辅助函数同样内置：

```javascript
var now_ts = now();
var utc_ts = date_utc("2024-01-01T12:00:00Z");
var text = format_date_utc(utc_ts, "%Y-%m-%d %H:%M:%S");
```

---

## 使用模块

TurboScript 可以通过模块（插件）扩展：

```javascript
// 加载 CSV 模块
import("csv");

// 读取 CSV 文件
var data = csv.read("prices.csv");

// 获取列
var close_prices = csv.col(data, "close");

// 处理数据
var average_price = avg(close_prices);
print("平均价格: " + average_price);
```

### 常用模块

```javascript
import("csv");      // CSV 解析
import("json");     // JSON 解析
import("ta");       // 技术分析
import("vec");      // 高级向量操作
import("net");      // HTTP/WebSocket
import("sqlite");   // 数据库访问
import("data_bind");// 二进制 TBE 模式 JIT 解析
```

---

## 示例：数据分析

让我们分析股票价格：

```javascript
import("csv");
import("ta");

// 读取数据
var data = csv.read("AAPL.csv");
var close = csv.col(data, "close");

// 计算指标
var sma20 = ta.sma(close, 20);    // 20 日均线
var sma50 = ta.sma(close, 50);    // 50 日均线
var rsi = ta.rsi(close, 14);      // RSI 指标

// 获取最新值
var latest_price = close[len(close) - 1];
var latest_rsi = rsi[len(rsi) - 1];

// 生成信号
var signal = "持有";
if (latest_rsi < 30) {
    signal = "买入 - 超卖";
} elif (latest_rsi > 70) {
    signal = "卖出 - 超买";
}

print("价格: " + latest_price);
print("RSI: " + latest_rsi);
print("信号: " + signal);
```

---

## 下一步

现在你已经掌握了基础知识：

1. **[语言指南](language-guide.md)** - 学习高级特性（解构、闭包、错误处理）
2. **[API 参考](api-reference.md)** - 探索所有内置函数
3. **[插件开发](plugin-development.md)** - 用 C/C++ 创建自己的模块

---

## 快速提示

### 管道操作符

优雅地链式操作：

```javascript
var result = data
    |> filter(x > 0)
    |> map(x => x * 2)
    |> sum();
```

### 可选链

安全地访问嵌套属性：

```javascript
var city = user?.address?.city;  // 如果任何部分为 null 则返回 null
```

### 解构

简洁地提取值：

```javascript
var [a, b, c] = [10, 20, 30];
var map{name, age} = person;
```

---

**准备深入学习？查看[语言指南](language-guide.md)！**
