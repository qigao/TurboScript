# CSV Filter Expression 语法说明

适用范围：

- `csv.filter(data, expr)`
- `csv.filter_count(data, expr)`
- `csv.filter_table(data, expr)`
- `stream.csv(path).filterExpr(expr)`
- `stream.csv(path).where(expr)`
- `turbo_dsv_filter_compile(filter, expr)`

## 1. 列命名规则

`dsv_filter` 约定：

- 数值列：`xxx_n`
- 字符串列：`xxx_s`

表达式里可以写：

- 去后缀名字：`price`, `name`
- 或原始列名：`price_n`, `name_s`

## 2. 顶层语法

单条子句：

- `LHS OP RHS`

多子句连接：

- `clause and clause`
- `clause or clause`

`and/or` 不区分大小写。

比较运算符：

- `==`
- `!=`
- `>`
- `>=`
- `<`
- `<=`

## 3. LHS（左侧）支持

### 3.1 简单列

- `price > 100`
- `name == "Alice"`

### 3.2 算术表达式（仅数值）

支持：

- `+ - * /`
- 一元正负号：`+x`, `-x`
- 括号与嵌套括号

示例：

- `a + b - 1 == 5`
- `a + -b == 2`
- `a * (b + c) == 14`
- `a * (b + (c - 1)) == 8`

优先级：

1. 括号
2. 一元正负号
3. `* /`
4. `+ -`

## 4. RHS（右侧）支持

- 数值：`123`, `-3.5`
- 字符串字面量：`"AAPL"`

## 5. 类型规则

- 字符串列不能参与算术。
- 算术表达式不能和字符串 RHS 比较。
- 字符串比较只允许 `LHS` 为字符串列（字节序比较）。
- 数值比较只允许 `LHS` 为数值列或数值算术表达式。

## 6. 当前限制

- `LHS` 的算术表达式必须至少包含一个列标识符（不接受纯常量 `1+2` 作为 LHS）。
- 不支持函数调用（如 `abs(x)`）作为过滤表达式的一部分。
- 除零不会在编译阶段拦截（运行时按 C `double` 行为处理）。

## 7. 常见错误消息（精确字符串）

- `invalid filter: expected column`
- `invalid filter: unknown column`
- `invalid filter: expected operator`
- `invalid filter: bad string literal`
- `invalid filter: expected value`
- `invalid filter: arithmetic on string column`
- `invalid filter: arithmetic requires numeric columns`
- `invalid filter: arithmetic cannot compare to string`
- `invalid filter: number on string column`
- `invalid filter: expected column or number after +/-`
- `invalid filter: unbalanced parentheses in arithmetic expression`
- `invalid filter: empty parentheses in arithmetic expression`
- `invalid filter: invalid character in arithmetic expression`
- `invalid filter: malformed arithmetic expression`
