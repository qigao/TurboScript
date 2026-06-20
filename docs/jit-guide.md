# TurboScript JIT 编译指南

## 概述

TurboScript 提供两种执行模式：

- **MIR 解释模式** (`turbo_script_run()`) - 稳定、功能完整、启动快，执行同一套 MIR lowering 生成的 MIR
- **MIR JIT 模式** (`turbo_script_run_jit()`) - 高性能、有编译开销，把同一套 MIR lowering 交给 MIR 生成原生代码

TurboScript 的运行时目标是“全部 MIR”：解释执行和 JIT 执行共享同一套 MIR lowering 与 runtime helper。JIT 不支持的形式应返回编译/验证错误，而不是静默回退到手写 AST 解释器。

### 统一执行模型

```text
script source
  -> lexer/parser
  -> AST
  -> validation
  -> AST-to-MIR lowering
  -> MIR module
       |-> turbo_script_run(): MIR interpreter
       |-> turbo_script_run_jit(): MIR native code generation
```

因此，“解释器”和“JIT”不是两套独立语义实现。二者共享同一份语法树、验证规则、MIR lowering 和 runtime helper，只是在最后一步选择 MIR 解释执行或 MIR 生成原生代码。TurboScript 对外运行路径以 MIR 为唯一后端。

---

## 快速开始

### 基础用法

```c
#include "turbo_script.h"

int main() {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    
    // 使用 JIT 模式执行
    const char *script = "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }";
    int ret = turbo_script_run_jit(ctx, script);
    
    if (ret == 0) {
        double result = ts_get_num(ctx, "sum");
        printf("Result: %g\n", result);
    } else {
        printf("Error: %s\n", turbo_script_get_error(ctx));
    }
    
    turbo_script_free(ctx);
    return 0;
}
```

### 选择执行模式

```c
// 计算密集型任务 - 使用 JIT
turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000000; i += 1) { sum += sin(i); }");

// 快速脚本、原型开发 - 使用 MIR 解释模式
turbo_script_run(ctx, "print('Hello, World!');");

// 明确需要忽略 JIT 编译成本的短脚本 - 使用 MIR 解释模式
turbo_script_run(ctx, "x = 1 + 2;");
```

---

## 性能特征

### 预期性能

基于 MIR 官方基准测试（Intel i5-13600K）：

| 指标 | JIT 模式 | 解释器模式 | GCC -O2 |
|------|---------|-----------|---------|
| 编译时间 | 336 μs | 22 μs | 27.1 ms |
| 执行速度 | **1.0x** | 0.07x (14x 慢) | 1.09x |
| 代码大小 | 961 KB | 240 KB | 32.2 MB |

**关键结论**：
- JIT 执行速度接近 GCC -O2（91% 性能）
- JIT 编译开销约 300 μs（可忽略）
- 解释器比 JIT 慢 10-15 倍

### 当前 TurboScript quick benchmark

以下数据来自 `bench_turbo_script_mir --quick` 的 Release 构建，用于观察 TurboScript 自身 lowering 与 runtime helper 的实际效果。具体数值会随机器、编译器和脚本变化，结论应以本地 benchmark 为准。

| 场景 | JIT warm 加速 | JIT exec-only 加速 | 说明 |
|------|---------------|--------------------|------|
| Pure loop | 14x+ | 14x+ | 标准数值循环 |
| Nested loop | 28x+ | 28x+ | 嵌套循环 |
| User function calls | 800x+ | 800x+ | 简单脚本函数已直接 MIR call |
| OOP direct field read/write | 17x+ | 19x+ | public numeric instance field slot fast path |
| OOP method field read/write | 16x+ | 20x+ | 方法 inline + field slot fast path |
| OOP inherited `super.method(...)` | 7x+ | 5x+ | 简单 `super` 调用链可 MIR inline |

### 性能建议

**适合 JIT 的场景**：
- ✅ 循环密集型计算（for/while 循环）
- ✅ 数学运算（sin/cos/sqrt 等）
- ✅ 向量/数组操作
- ✅ 重复执行的脚本（编译一次，多次执行）

**适合 MIR 解释模式的场景**：
- ✅ 一次性脚本（编译开销不值得）
- ✅ 快速原型开发（无需等待编译）
- ✅ 调试阶段（不生成机器码，定位更直接）

---

## 功能支持

### ✅ 完全支持的特性

以下特性在 JIT 模式下完全支持，性能优秀：

#### 1. 基础表达式
```javascript
// 算术运算
x = (a + b) * c - d / e;
y = 2 ^ 10;  // 幂运算
z = 10 % 3;  // 取模

// 比较运算
result = (x > 5) && (y < 10) || (z == 0);

// 一元运算
neg = -x;
flag = !condition;
```

#### 2. 控制流
```javascript
// if/else
if (x > 0) {
    y = 1;
} else if (x < 0) {
    y = -1;
} else {
    y = 0;
}

// for 循环
for (i = 0; i < 100; i += 1) {
    sum += i;
}

// while 循环
while (x > 0) {
    x -= 1;
}

// do-while 循环
do {
    x += 1;
} while (x < 10);

// break/continue
for (i = 0; i < 100; i += 1) {
    if (i == 50) break;
    if (i % 2 == 0) continue;
    sum += i;
}

// switch
switch (x) {
    case 1: { result = 10; }
    case 2: { result = 20; }
    default: { result = 0; }
}
```

#### 3. 函数调用
```javascript
// 数学函数（直接调用 C 标准库，无开销）
x = sin(0.5);
y = cos(1.0);
z = sqrt(144);
w = abs(-42);

// 用户定义函数
func double(x) {
    return x * 2;
}
result = double(21);  // 42

// 内建函数
print("Hello, World!");
```

#### 4. 数据结构
```javascript
// 向量索引（原生指针访问，极快）
ts_bind_vec(ctx, "prices", data, 1000);  // C 侧绑定
avg = 0;
for (i = 0; i < 1000; i += 1) {
    avg += prices[i];
}
avg /= 1000;

// Map 访问（指针缓存优化）
config = map { width: 1920, height: 1080 };
area = config.width * config.height;

// 成员访问
point = map { x: 3, y: 4 };
dist = (point.x * point.x + point.y * point.y) ^ 0.5;
```

#### 5. 高级特性
```javascript
// 三元运算符
result = (x > 0) ? 1 : -1;

// For-in 循环
nums = [1, 2, 3, 4, 5];
sum = 0;
for (x in nums) {
    sum += x;
}

// 常量
const PI = 3.14159;
area = PI * r * r;
```

---

### ⚠️ 语义支持但可能走 runtime helper 的特性

以下特性已纳入 MIR 路径，但通常会调用 runtime helper。它们和 MIR 解释模式保持同一语义，但性能不等同纯数值表达式、内联数学函数或预绑定向量 fast path。

#### 1. 字符串和动态值
```javascript
// 支持，但通常通过 runtime helper 处理
str = "Value: " + x;
msg = `Value: ${x}`;
```

#### 2. 闭包与捕获变量
```javascript
func makeCounter() {
    var count = 0;
    return func() {
        count += 1;
        return count;
    };
}
counter = makeCounter();
```

#### 3. 高阶函数
```javascript
func map(arr, fn) {
    result = [];
    for (x in arr) {
        result.push(fn(x));
    }
    return result;
}
doubled = map([1, 2, 3], func(x) { return x * 2; });
```

---

### ❌ 必须显式拒绝的特性

以下形式如果尚未被 MIR lowering 覆盖，必须返回错误，不允许自动回退：

1. 动态代码生成
2. 未纳入语法与验证规则的反射/元编程
3. 任何尚未纳入 MIR lowering 或 runtime helper 合约的节点组合

---

## 不支持特性的处理

### 显式拒绝

当 JIT 编译器遇到不支持的特性时，应返回编译或验证错误。这样可以保证调用方明确知道当前脚本没有进入 JIT 路径，也避免解释器执行被误认为 JIT 性能。

```c
int ret = turbo_script_run_jit(ctx, script);
if (ret != 0) {
    // 选择之一：
    // 1. 修正脚本，让它进入 JIT 支持范围
    // 2. 明确改用解释器路径
    printf("JIT compile error: %s\n", turbo_script_get_error(ctx));
}
```

### 保持 JIT 路径清晰

1. **使用简单表达式**
   ```javascript
   // 好：纯数值计算
   result = (a + b) * c;
   
   // 可用但不是最快路径：混合类型和字符串拼接
   result = "Value: " + (a + b);
   ```

2. **把闭包移出热循环**
   ```javascript
   // 好：热循环里只做数值调用
   func process(x, state) {
       return x * state;
   }
   
   // 可用但通常通过 helper 路径：闭包捕获
   func makeProcessor(state) {
       return func(x) { return x * state; };
   }
   ```

3. **使用预绑定数据**
   ```javascript
   // 好：C 侧预绑定向量
   ts_bind_vec(ctx, "data", array, size);
   sum = 0;
   for (i = 0; i < size; i += 1) {
       sum += data[i];  // 原生指针访问，极快
   }
   
   // 可用但通常不是最快路径：脚本侧创建向量
   data = [1, 2, 3, ...];
   ```

---

## 高级用法

### 公开编译对象

`turbo_script_compile()` 当前提供 source-backed compiled object：它会解析和验证脚本，后续通过 `turbo_script_exec()` 使用 MIR 解释模式执行。需要真正的 JIT 编译/执行分离时，使用下方内部 API 或依赖 `turbo_script_run_jit()` 的 JIT 缓存。

```c
// 解析并验证一次，保存脚本文本
turbo_script_compiled_t *compiled = turbo_script_compile(ctx, script);
if (!compiled) {
    fprintf(stderr, "Compile error: %s\n", turbo_script_get_error(ctx));
    return -1;
}

// 多次执行；当前公开 API 走 MIR 解释模式
for (int i = 0; i < 1000; i++) {
    ts_bind_num(ctx, "input", i);
    turbo_script_exec(ctx, compiled);
    double result = ts_get_num(ctx, "output");
    printf("Result[%d]: %g\n", i, result);
}

turbo_script_compiled_free(compiled);
```

### JIT 缓存

TurboScript 内部实现了 JIT 缓存（64 个槽位）：

```c
// 首次执行：编译 + 执行
turbo_script_run_jit(ctx, "x = 42;");  // ~300 μs 编译 + 执行

// 再次执行：缓存命中，无编译
turbo_script_run_jit(ctx, "x = 42;");  // ~1 μs 执行
```

**缓存策略**：
- 基于脚本内容的哈希值
- LRU 替换策略
- 64 个槽位（可配置）

### 内部 API（高级用户）

```c
// 仅编译，不执行
int turbo_script_compile_mir(turbo_script_ctx_t *ctx, const char *script);

// 执行上次编译的代码
int turbo_script_exec_jit(turbo_script_ctx_t *ctx);

// 示例：预编译多个脚本
turbo_script_compile_mir(ctx, script1);
void *fn1 = ctx->mir_last_fn;

turbo_script_compile_mir(ctx, script2);
void *fn2 = ctx->mir_last_fn;

// 后续直接调用函数指针（需要理解 MIR 调用约定）
```

---

## 调试与诊断

### 错误处理

```c
int ret = turbo_script_run_jit(ctx, script);
if (ret != 0) {
    turbo_script_error_code_t code = turbo_script_get_error_code(ctx);
    const char *msg = turbo_script_get_error(ctx);
    
    switch (code) {
        case TURBO_SCRIPT_ERROR_PARSE:
            fprintf(stderr, "Parse error: %s\n", msg);
            break;
        case TURBO_SCRIPT_ERROR_VALIDATE:
            fprintf(stderr, "Validation error: %s\n", msg);
            break;
        case TURBO_SCRIPT_ERROR_JIT:
            fprintf(stderr, "JIT compile error: %s\n", msg);
            // 宿主可以显式选择解释器路径，但这不是 JIT 自动行为。
            // turbo_script_run(ctx, script);
            break;
        case TURBO_SCRIPT_ERROR_RUNTIME:
            fprintf(stderr, "Runtime error: %s\n", msg);
            break;
        default:
            fprintf(stderr, "Unknown error: %s\n", msg);
    }
}
```

### 性能分析

```c
#include <time.h>

double benchmark(turbo_script_ctx_t *ctx, const char *script, int iterations) {
    clock_t start = clock();
    for (int i = 0; i < iterations; i++) {
        turbo_script_run_jit(ctx, script);
    }
    clock_t end = clock();
    return (double)(end - start) / CLOCKS_PER_SEC;
}

// 使用
double time_jit = benchmark(ctx, script, 1000);
printf("JIT: %.3f ms per iteration\n", time_jit * 1000 / 1000);
```

### 对比测试

```c
// 对比 JIT vs 解释器
turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

const char *script = "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }";

double time_jit = benchmark(ctx_jit, script, 100);
double time_interp = benchmark_interp(ctx_interp, script, 100);

printf("JIT: %.3f ms\n", time_jit * 1000);
printf("Interpreter: %.3f ms\n", time_interp * 1000);
printf("Speedup: %.2fx\n", time_interp / time_jit);

turbo_script_free(ctx_jit);
turbo_script_free(ctx_interp);
```

---

## 平台支持

### 支持的平台

MIR JIT 后端支持以下平台：

| 平台 | 架构 | 状态 |
|------|------|------|
| Linux | x86_64 | ✅ 完全支持 |
| Linux | aarch64 | ✅ 完全支持 |
| Linux | ppc64le | ✅ 完全支持 |
| Linux | s390x | ✅ 完全支持 |
| Linux | riscv64 | ✅ 完全支持 |
| macOS | x86_64 | ✅ 完全支持 |
| macOS | aarch64 (M1/M2) | ✅ 完全支持 |
| Windows | x86_64 | ✅ 完全支持 |

### 不支持平台

在不支持 MIR JIT 的平台上，宿主应显式选择解释器模式：

```c
int ret = turbo_script_run_jit(ctx, script);
if (ret == -1 && turbo_script_get_error_code(ctx) == TURBO_SCRIPT_ERROR_JIT) {
    // JIT 不可用；调用方明确选择解释器。
    ret = turbo_script_run(ctx, script);
}
```

---

## 常见问题

### Q1: JIT 模式比解释器慢？

**A**: 可能原因：
1. 脚本太短，编译开销大于执行收益
2. 热路径包含 JIT 不支持节点或 runtime helper 调用（检查 `test_turbo_script_mir`）
3. 首次执行包含编译时间（使用 `turbo_script_compile()` 分离编译）

**解决方案**：
```c
// 测量纯执行时间
turbo_script_compiled_t *compiled = turbo_script_compile(ctx, script);
double time = benchmark_exec(ctx, compiled, 1000);
```

### Q2: 如何知道脚本没有进入 JIT？

**A**: 检查返回值和错误码：
1. `turbo_script_run_jit()` 返回非 0 表示没有成功进入 JIT 执行
2. `turbo_script_get_error_code(ctx) == TURBO_SCRIPT_ERROR_JIT` 表示 JIT 编译或 lowering 失败
3. 错误日志会说明 JIT 编译失败原因

**未来计划**：添加 `--jit-stats` 标志输出诊断信息。

### Q3: 可以禁用 JIT 吗？

**A**: 可以，始终使用 `turbo_script_run()` 即可：
```c
// 强制使用解释器
turbo_script_run(ctx, script);  // 永不 JIT
```

### Q4: JIT 模式线程安全吗？

**A**: 每个 `turbo_script_ctx_t` 持有独立的 MIR 上下文，支持多线程：
```c
// 每线程一个上下文 - 安全
#pragma omp parallel
{
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_run_jit(ctx, script);
    turbo_script_free(ctx);
}

// 共享上下文 - 不安全，需要加锁
turbo_script_ctx_t *shared_ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
#pragma omp parallel
{
    #pragma omp critical
    turbo_script_run_jit(shared_ctx, script);  // 需要锁保护
}
```

---

## 最佳实践

### ✅ 推荐做法

1. **计算密集型任务使用 JIT**
   ```c
   // 大量循环计算
   turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000000; i += 1) { sum += sin(i); }");
   ```

2. **预绑定数据**
   ```c
   // C 侧绑定向量，脚本侧高效访问
   ts_bind_vec(ctx, "prices", data, size);
   turbo_script_run_jit(ctx, "avg = 0; for (i = 0; i < prices.length; i += 1) { avg += prices[i]; }");
   ```

3. **分离编译与执行**
   ```c
   // 编译一次，执行多次
   turbo_script_compiled_t *compiled = turbo_script_compile(ctx, script);
   for (int i = 0; i < 1000; i++) {
       turbo_script_exec(ctx, compiled);
   }
   ```

4. **性能测试**
   ```c
   // 始终对比 JIT vs 解释器
   double time_jit = benchmark_jit(ctx, script);
   double time_interp = benchmark_interp(ctx, script);
   if (time_jit < time_interp * 0.8) {
       printf("JIT is beneficial\n");
   }
   ```

### ❌ 避免做法

1. **短脚本使用 JIT**
   ```c
   // 差：编译开销 > 执行收益
   turbo_script_run_jit(ctx, "x = 1 + 1;");
   
   // 好：使用解释器
   turbo_script_run(ctx, "x = 1 + 1;");
   ```

2. **混合类型操作**
   ```c
   // 差：JIT 可能拒绝
   turbo_script_run_jit(ctx, "result = 'Value: ' + (a + b);");
   
   // 好：分离数值计算与字符串操作
   turbo_script_run_jit(ctx, "result = a + b;");
   turbo_script_run(ctx, "str = 'Value: ' + result;");
   ```

3. **忽略错误**
   ```c
   // 差：不检查返回值
   turbo_script_run_jit(ctx, script);
   
   // 好：检查错误，并由宿主显式决定是否使用解释器
   if (turbo_script_run_jit(ctx, script) != 0) {
       turbo_script_run(ctx, script);
   }
   ```

---

## 参考资料

- [MIR 项目主页](https://github.com/vnmakarov/mir)
- [MIR 性能基准](https://github.com/vnmakarov/mir#current-mir-performance-data)
- [TurboScript API 参考](api-reference.md)
- [TurboScript 语言指南](language-guide.md)

---

**最后更新**: 2026-05-19  
**版本**: 1.0.0
