# TurboScript JIT 快速参考卡

**版本**: 1.0.0 | **最后更新**: 2026-05-19

---

## 🚀 快速开始

### 基础用法
```c
turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }");
double result = ts_get_num(ctx, "sum");
turbo_script_free(ctx);
```

### 选择执行模式
| 场景 | 使用 | 原因 |
|------|------|------|
| 计算密集型 | `turbo_script_run_jit()` | MIR native code，适合热循环 |
| 一次性脚本 | `turbo_script_run()` | 同一份 MIR，用 MIR interpreter 执行，无 native code 编译开销 |
| 字符串/动态值 | 视脚本选择 | 已纳入 MIR helper 路径，但不等同纯数值 fast path |
| 闭包/高阶函数 | 视脚本选择 | 支持范围内走 MIR；未覆盖形式显式报错 |

---

## ✅ 功能支持矩阵

| 特性 | MIR JIT | MIR 解释器 | 性能 |
|------|-----|--------|------|
| **表达式** |
| 算术运算 (+, -, *, /, %, ^) | ✅ | ✅ | 极快 |
| 比较运算 (==, !=, <, >, <=, >=) | ✅ | ✅ | 极快 |
| 逻辑运算 (&&, \|\|, !) | ✅ | ✅ | 极快 |
| **控制流** |
| if/else | ✅ | ✅ | 快 |
| for/while/do-while | ✅ | ✅ | 极快 |
| break/continue | ✅ | ✅ | 快 |
| switch | ✅ | ✅ | 快 |
| return | ✅ | ✅ | 快 |
| **函数** |
| 函数定义 | ✅ | ✅ | 快 |
| 函数调用 | ✅ | ✅ | 快 |
| 数学函数 (sin/cos/sqrt) | ✅ | ✅ | 极快 |
| 递归 | ✅ | ✅ | 中等 |
| 闭包 | ⚠️ | ✅ | helper/受限 |
| 高阶函数 | ⚠️ | ✅ | helper/受限 |
| **数据结构** |
| 向量索引 (预绑定) | ✅ | ✅ | 极快 |
| 向量索引 (脚本创建) | ⚠️ | ✅ | 中等 |
| Map 访问 | ✅ | ✅ | 快 |
| 嵌套 Map | ⚠️ | ✅ | 中等 |
| **字符串** |
| 字符串拼接 | ⚠️ | ✅ | helper |
| 字符串比较 | ⚠️ | ✅ | helper |
| **高级特性** |
| 三元运算符 (?:) | ✅ | ✅ | 快 |
| For-in 循环 | ✅ | ✅ | 快 |
| 常量 (const) | ✅ | ✅ | 快 |
| 异常处理 | ⚠️ | ✅ | helper |
| async/await 语法糖 | ⚠️ | ✅ | helper |

**图例**: ✅ 完全支持 | ⚠️ 部分支持/受限 | ❌ 不支持；JIT 不支持的形式应显式报错，不应静默回退

---

## ⚡ 性能数据

### 预期加速比（JIT vs 解释器）

| 场景 | 加速比 | 示例 |
|------|--------|------|
| 纯循环计算 | 14x+ | `for (i = 0; i < 100000; i++) { sum += i; }` |
| 数学函数 | 8-12x | `for (i = 0; i < 10000; i++) { sum += sin(i); }` |
| 向量访问（预绑定） | 10x+ | `for (i = 0; i < 10000; i++) { sum += data[i]; }` |
| 条件分支 | 8x+ | `if (x > 0) { y = 1; } else { y = -1; }` |
| 函数调用 | 800x+ | `func double(x) { return x * 2; }` |
| OOP 字段读写 | 17x+ | `c.value = c.value + 1;` |
| OOP `super.method(...)` | 7x+ | `return super.value(x) + 1;` |
| 字符串操作 | helper 路径 | `str = "Hello" + " World";` |

### 编译开销
- **首次编译**: ~300 μs
- **缓存命中**: ~1 μs
- **缓存大小**: 64 个槽位

---

## 🎯 最佳实践

### ✅ 推荐做法

#### 1. 预绑定向量
```c
// 好：C 侧绑定，JIT 原生指针访问
double data[1000];
ts_bind_vec(ctx, "data", data, 1000);
turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000; i++) { sum += data[i]; }");
```

#### 2. 分离编译与执行
```c
// 好：编译一次，执行多次
turbo_script_compiled_t *compiled = turbo_script_compile(ctx, script);
for (int i = 0; i < 1000; i++) {
    ts_bind_num(ctx, "input", i);
    turbo_script_exec(ctx, compiled);
}
turbo_script_compiled_free(compiled);
```

#### 3. 使用简单表达式
```c
// 好：纯数值计算
turbo_script_run_jit(ctx, "result = (a + b) * c;");
```

### ❌ 避免做法

#### 1. 短脚本使用 JIT
```c
// 差：编译开销 > 执行收益
turbo_script_run_jit(ctx, "x = 1 + 1;");

// 好：使用解释器
turbo_script_run(ctx, "x = 1 + 1;");
```

#### 2. 混合类型操作
```c
// 差：JIT 可能拒绝
turbo_script_run_jit(ctx, "result = 'Value: ' + (a + b);");

// 好：分离数值计算与字符串操作
turbo_script_run_jit(ctx, "result = a + b;");
turbo_script_run(ctx, "str = 'Value: ' + result;");
```

#### 3. 闭包在热路径
```c
// 差：闭包不适合 JIT 热路径
turbo_script_run_jit(ctx, 
    "func makeAdder(x) { return func(y) { return x + y; }; }"
    "for (i = 0; i < 1000; i++) { add5 = makeAdder(5); result = add5(i); }");

// 好：无闭包版本
turbo_script_run_jit(ctx,
    "func add(x, y) { return x + y; }"
    "for (i = 0; i < 1000; i++) { result = add(5, i); }");
```

---

## 🔍 调试与诊断

### 检查 JIT 是否生效
```c
int ret = turbo_script_run_jit(ctx, script);
if (ret != 0) {
    turbo_script_error_code_t code = turbo_script_get_error_code(ctx);
    if (code == TURBO_SCRIPT_ERROR_JIT) {
        printf("JIT compilation failed: %s\n", turbo_script_get_error(ctx));
        // 宿主可以显式选择解释器路径。
        turbo_script_run(ctx, script);
    }
}
```

### 性能对比
```c
// 测量 JIT 性能
clock_t start = clock();
for (int i = 0; i < 1000; i++) {
    turbo_script_run_jit(ctx, script);
}
double time_jit = (double)(clock() - start) / CLOCKS_PER_SEC;

// 测量解释器性能
start = clock();
for (int i = 0; i < 1000; i++) {
    turbo_script_run(ctx, script);
}
double time_interp = (double)(clock() - start) / CLOCKS_PER_SEC;

printf("Speedup: %.2fx\n", time_interp / time_jit);
```

### 内存泄漏检测
```bash
# Valgrind
valgrind --leak-check=full ./your_program

# AddressSanitizer
cmake -DCMAKE_C_FLAGS="-fsanitize=address" ..
./your_program
```

---

## 🐛 常见问题

### Q: JIT 比解释器慢？
**A**: 可能原因：
1. 脚本太短（编译开销 > 执行收益）
2. 热路径包含 JIT 不支持节点或 helper 调用（检查 `test_turbo_script_mir`）
3. 首次执行（包含编译时间）

**解决方案**:
```c
// 分离编译与执行
turbo_script_compiled_t *compiled = turbo_script_compile(ctx, script);
// 测量纯执行时间
clock_t start = clock();
turbo_script_exec(ctx, compiled);
double time = (double)(clock() - start) / CLOCKS_PER_SEC;
```

### Q: 如何知道脚本没有进入 JIT？
**A**: 检查返回值和错误码：
```c
int ret = turbo_script_run_jit(ctx, script);
if (ret != 0 && turbo_script_get_error_code(ctx) == TURBO_SCRIPT_ERROR_JIT) {
    printf("JIT failed: %s\n", turbo_script_get_error(ctx));
}
```

### Q: 线程安全吗？
**A**: 每线程一个上下文：
```c
// 安全：每线程独立上下文
#pragma omp parallel
{
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_run_jit(ctx, script);
    turbo_script_free(ctx);
}
```

---

## 📊 性能基准测试

### 运行基准测试
```bash
cd build/bin
./bench_turbo_script_mir
```

### 性能回归检查
```bash
cd turbo_script/test
./check_performance_regression.sh
```

### 预期输出
```
=== Pure Loop (100k iterations) ===
Results:
  JIT:         245.123 ms total, 2.451230 ms/iter
  Interpreter: 3421.567 ms total, 34.215670 ms/iter
  Speedup:     13.96x
  Result:      4999950000.00 (verified)
```

---

## 🔗 相关资源

| 资源 | 链接 |
|------|------|
| 完整 JIT 指南 | [docs/jit-guide.md](jit-guide.md) |
| API 参考 | [docs/api-reference.md](api-reference.md) |
| 测试指南 | [turbo_script/test/README.md](../turbo_script/test/README.md) |
| MIR 项目 | [github.com/vnmakarov/mir](https://github.com/vnmakarov/mir) |
| 审查报告 | [.codex/review_mir_jit_integration.md](../.codex/review_mir_jit_integration.md) |

---

## 📝 速查表

### API 函数
```c
// 初始化
turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

// 执行（JIT）
int turbo_script_run_jit(turbo_script_ctx_t *ctx, const char *script);

// 执行（解释器）
int turbo_script_run(turbo_script_ctx_t *ctx, const char *script);

// 编译
turbo_script_compiled_t *turbo_script_compile(turbo_script_ctx_t *ctx, const char *script);

// 执行已编译代码
int turbo_script_exec(turbo_script_ctx_t *ctx, turbo_script_compiled_t *compiled);

// 绑定数据
void ts_bind_num(turbo_script_ctx_t *ctx, const char *name, double value);
void ts_bind_str(turbo_script_ctx_t *ctx, const char *name, const char *value);
int ts_bind_vec(turbo_script_ctx_t *ctx, const char *name, const double *data, size_t len);

// 获取结果
double ts_get_num(turbo_script_ctx_t *ctx, const char *name);
const char *ts_get_str(turbo_script_ctx_t *ctx, const char *name);
int ts_get_vec(turbo_script_ctx_t *ctx, const char *name, const double **data, size_t *len);

// 错误处理
const char *turbo_script_get_error(turbo_script_ctx_t *ctx);
turbo_script_error_code_t turbo_script_get_error_code(turbo_script_ctx_t *ctx);

// 清理
void turbo_script_compiled_free(turbo_script_compiled_t *compiled);
void turbo_script_free(turbo_script_ctx_t *ctx);
```

### 错误码
```c
TURBO_SCRIPT_ERROR_NONE      // 无错误
TURBO_SCRIPT_ERROR_ARGUMENT  // 参数错误
TURBO_SCRIPT_ERROR_PARSE     // 解析错误
TURBO_SCRIPT_ERROR_VALIDATE  // 验证错误
TURBO_SCRIPT_ERROR_RUNTIME   // 运行时错误
TURBO_SCRIPT_ERROR_JIT       // JIT 编译错误
TURBO_SCRIPT_ERROR_OOM       // 内存不足
```

---

**打印此页作为快速参考！**
