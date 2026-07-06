# TurboScript MIR 后端模块化设计

> **注**：此文档为模板，在完成 Phase 1.1 重构后迁移到 `src/mir/README.md`

## 模块总览

MIR 后端负责将 TurboScript AST 转换为 MIR 中间表示，并支持解释执行或 JIT 编译。

```
┌─────────────────────────────────────────────────────────────┐
│                     TurboScript Frontend                     │
│              (exprtk: lexer/parser/validator)                │
└───────────────────────┬─────────────────────────────────────┘
                        │ AST
                        ↓
┌─────────────────────────────────────────────────────────────┐
│                   MIR Backend Pipeline                       │
├─────────────────────────────────────────────────────────────┤
│  1. Compiler Framework (mir_compiler.c)                     │
│     - 上下文管理、寄存器分配、变量表                           │
│                                                              │
│  2. AST Lowering (mir_lowering.c)                           │
│     - AST 节点 → MIR 指令转换                                │
│     - 表达式、语句、控制流                                    │
│                                                              │
│  3. Optimization (mir_optimize.c)                           │
│     - 数值循环直接路径                                        │
│     - 向量/Map/OOP 指针缓存                                   │
│     - 单态方法调用优化                                        │
│                                                              │
│  4. Runtime Bridge (mir_runtime.c)                          │
│     - 动态值操作 helper 调用                                  │
│     - OOP/字符串/Map runtime 桥接                            │
│                                                              │
│  5. Closure Compilation (mir_closure.c)                     │
│     - 闭包捕获分析                                            │
│     - 预编译简单闭包                                          │
│     - 复杂闭包回退到 runtime                                  │
│                                                              │
│  6. JIT Cache (mir_cache.c)                                 │
│     - 脚本哈希与缓存查找                                      │
│     - LRU 淘汰策略                                            │
└───────────────────────┬─────────────────────────────────────┘
                        │ MIR Module
                        ↓
          ┌─────────────┴─────────────┐
          │                           │
    MIR Interpreter              MIR JIT
    (turbo_script_run)    (turbo_script_run_jit)
```

---

## 模块职责详解

### 1. 编译器框架 (`mir_compiler.c`)

**核心职责**：
- `ts_mir_compiler_t` 上下文初始化/销毁
- 寄存器分配（`new_temp_reg`, `new_temp_ireg`, `new_temp_preg`）
- 变量表管理（`get_or_create_reg`）
- 变量脏标记与同步策略
- 编译栈帧管理（`ts_mir_capture_frame`, `ts_mir_restore_frame`）

**关键数据结构**：
```c
typedef struct {
  MIR_context_t ctx;               // MIR 编译器上下文
  MIR_item_t func;                 // 当前编译的 MIR 函数
  ts_mir_var_entry_t **vars;       // 变量寄存器映射表
  int var_count, var_capacity;
  int tmp_count;                   // 临时寄存器计数器
  loop_frame_t loop_stack[32];     // break/continue 标签栈
  int loop_depth;                  // 循环嵌套深度
  ts_mir_externals_t ext;          // Runtime helper 导入缓存
  // ... 其他字段见 turbo_script_mir_internal.h
} ts_mir_compiler_t;
```

**关键函数**：
- `ts_mir_compiler_init()` - 初始化编译器上下文
- `ts_mir_compiler_free()` - 释放编译器资源
- `get_or_create_reg(compiler, var_name)` - 获取或创建变量寄存器
- `new_temp_reg(compiler)` - 分配临时 double 寄存器
- `ts_mir_mark_var_dirty(compiler, var_name)` - 标记变量为脏（需同步）
- `ts_mir_sync_all_dirty_vars(compiler)` - 同步所有脏变量到环境

**变量同步策略**：
```c
// 写入变量时标记为脏
ts_mir_mark_var_dirty(c, "x");

// 在以下位置强制同步：
// 1. 控制流合并点（try/catch, return, break/continue）
// 2. 函数调用前（动态函数可能读取环境）
// 3. 函数尾声（epilogue）
ts_mir_sync_all_dirty_vars(c);
```

---

### 2. AST Lowering (`mir_lowering.c`)

**核心职责**：
- 将 AST 节点递归转换为 MIR 指令
- 处理表达式（算术、逻辑、比较、成员访问）
- 处理语句（赋值、控制流、函数定义）
- 常量折叠与类型推导

**主入口函数**：
```c
// 递归 lower AST 节点，返回结果寄存器
MIR_reg_t ts_mir_lower_node(ts_mir_compiler_t *c, 
                            exprtk_node_t *node);
```

**Lowering 规则示例**：

| AST 节点类型 | MIR 指令 | 说明 |
|-------------|---------|------|
| `exprtk_TOKEN_NUMBER` | `MIR_DMOV` | 直接加载常量 |
| `exprtk_TOKEN_PLUS` | `MIR_DADD` | 数值相加 |
| `exprtk_TOKEN_MINUS` | `MIR_DSUB` | 数值相减 |
| `exprtk_TOKEN_MUL` | `MIR_DMUL` | 数值相乘 |
| `exprtk_TOKEN_DIV` | `MIR_DDIV` | 数值相除 |
| `exprtk_TOKEN_LT` | `MIR_DLT` → `MIR_BEQ` | 比较跳转 |
| `exprtk_TOKEN_IDENTIFIER` | `load_var(ctx, "name")` | 从环境加载 |
| `exprtk_TOKEN_ASSIGN` | `store_var(ctx, "name", val)` | 存储到环境 |

**优化判定**：
```c
// 数值表达式 → 直接 MIR 指令
if (ts_mir_is_numeric_expr(node)) {
  return ts_mir_lower_numeric_expr(c, node);
}

// 动态值 → Runtime helper 调用
return ts_mir_call_runtime_helper(c, "value_expr", node);
```

---

### 3. 优化路径 (`mir_optimize.c`)

**核心职责**：
- 识别可优化的热点模式
- 生成快速路径代码
- 管理指针缓存（向量、Map、OOP）

**优化策略**：

#### 3.1 数值循环优化
```typescript
// 源码
for (i = 0; i < 1000000; i += 1) {
  sum += sin(i);
}

// 优化路径：
// - i, sum 使用寄存器
// - sin() 直接调用 C 标准库
// - 无动态查表
```

#### 3.2 向量指针缓存
```typescript
// 源码
for (i = 0; i < vec.length; i += 1) {
  sum += vec[i];
}

// 优化路径：
// - 缓存 vec 的 double* 指针
// - 使用原生指针访问（避免 runtime 调用）
```

**实现**：
```c
typedef struct {
  const char *name;      // 向量变量名
  MIR_reg_t ptr_reg;     // 缓存的指针寄存器
} ts_mir_vec_ptr_entry_t;

// 获取或创建向量指针缓存
MIR_reg_t ts_mir_get_or_add_vec_ptr(ts_mir_compiler_t *c, 
                                    const char *vec_name);
```

#### 3.3 OOP Slot 缓存
```typescript
// 源码
class Point { x: number; y: number; }
let p = new Point(1, 2);
for (i = 0; i < 1000000; i += 1) {
  sum += p.x;  // 热点：重复访问 public 字段
}

// 优化路径：
// - 缓存 p.x 的 slot 索引
// - 直接访问 instance->slots[index]
```

**实现**：
```c
typedef struct {
  exprtk_instance_t *instance;  // 对象实例
  size_t index;                 // 字段 slot 索引
} ts_mir_oop_slot_cache_t;

// 生成缓存查找 + 直接访问
MIR_reg_t ts_mir_oop_cached_field_get(ts_mir_compiler_t *c,
                                      const char *obj_name,
                                      const char *member_name);
```

---

### 4. Runtime 桥接 (`mir_runtime.c`)

**核心职责**：
- 初始化 `ts_mir_externals_t`（runtime helper 导入缓存）
- 生成 runtime helper 调用
- 处理动态值操作

**Runtime Helper 分类**：

| 类别 | Helper 函数 | 说明 |
|------|------------|------|
| **环境操作** | `ts_mir_load_var`, `ts_mir_store_var` | 变量加载/存储 |
| **函数调用** | `ts_mir_call0..calln`, `ts_mir_call_native` | 动态函数调用 |
| **向量操作** | `ts_mir_vec_get`, `ts_mir_vec_assign` | 向量索引读写 |
| **Map 操作** | `ts_mir_map_get_key`, `ts_mir_map_assign` | Map 键值读写 |
| **OOP 操作** | `ts_mir_oop_new`, `ts_mir_oop_member_call` | 对象创建、方法调用 |
| **字符串/模板** | `ts_mir_string_assign`, `ts_mir_template_assign` | 字符串操作 |

**Helper 调用示例**：
```c
// 生成 runtime helper 调用：load_var(ctx, "x")
MIR_reg_t ts_mir_emit_load_var(ts_mir_compiler_t *c, const char *var_name) {
  MIR_reg_t result = new_temp_reg(c);
  MIR_reg_t name_reg = new_temp_preg(c);
  
  // 加载变量名指针
  MIR_append_insn(c->ctx, c->func,
    MIR_new_insn(c->ctx, MIR_MOV,
      MIR_new_reg_op(c->ctx, name_reg),
      MIR_new_ref_op(c->ctx, var_name)));
  
  // 调用 runtime helper
  MIR_append_insn(c->ctx, c->func,
    MIR_new_call_insn(c->ctx, 4,
      MIR_new_ref_op(c->ctx, c->ext.load_var_import),
      MIR_new_reg_op(c->ctx, result),
      MIR_new_reg_op(c->ctx, c->ctx_reg),
      MIR_new_reg_op(c->ctx, name_reg)));
  
  return result;
}
```

---

### 5. 闭包编译 (`mir_closure.c`)

**核心职责**：
- 分析函数体中的捕获变量
- 为简单闭包生成独立 MIR 函数
- 为复杂闭包回退到 runtime hook

**闭包分析流程**：
```c
ts_closure_analysis_t *ts_analyze_closure(
  exprtk_node_t *func_body,
  exprtk_node_t **arg_params,
  size_t arg_count,
  exprtk_env_t *closure_env
);

// 结果：
// - captured_vars: ["x", "y"]
// - captured_count: 2
// - can_jit: true/false
// - reason: "nested closure" / NULL
```

**预编译条件**：
```c
bool can_precompile_closure(ts_closure_analysis_t *analysis) {
  return analysis->can_jit &&
         analysis->captured_count <= 10 &&
         // 其他条件...
}
```

**编译策略**：
```c
if (can_precompile_closure(analysis)) {
  // 生成独立 MIR 函数，捕获变量作为额外参数
  MIR_item_t closure_func = 
    ts_mir_compile_closure_function(c, func_body, analysis);
} else {
  // 回退到 runtime hook
  ts_mir_emit_runtime_closure(c, func_body, analysis);
}
```

详细设计见 `docs/CLOSURE_GUIDE.md`（Phase 2.1 交付）。

---

### 6. JIT 缓存 (`mir_cache.c`)

**核心职责**：
- 基于脚本哈希的缓存查找
- LRU 淘汰策略
- 哈希碰撞处理（字符串副本验证）

**缓存结构**：
```c
struct {
  uint64_t hash;           // 脚本哈希值
  void *fn_ptr;            // 编译后的原生函数指针
  uint32_t access_count;   // LRU 访问计数
  char *script;            // 脚本副本（防碰撞）
} jit_cache[TS_JIT_CACHE_SIZE];  // 默认 128 槽位
```

**查找流程**：
```c
void *ts_mir_cache_lookup(turbo_script_ctx_t *ctx, const char *script) {
  uint64_t hash = ts_compute_hash(script);
  int slot = hash % TS_JIT_CACHE_SIZE;
  
  if (ctx->jit_cache[slot].hash == hash) {
    // 验证脚本内容（防碰撞）
    if (strcmp(ctx->jit_cache[slot].script, script) == 0) {
      ctx->jit_stats.cache_hit_count++;
      ctx->jit_cache[slot].access_count++;  // LRU
      return ctx->jit_cache[slot].fn_ptr;
    } else {
      ctx->jit_stats.hash_collision_count++;
    }
  }
  
  ctx->jit_stats.cache_miss_count++;
  return NULL;
}
```

**淘汰策略**：
```c
// 找到最小访问计数的槽位（LRU）
int ts_mir_cache_find_victim_slot(turbo_script_ctx_t *ctx) {
  int min_slot = 0;
  uint32_t min_count = UINT32_MAX;
  
  for (int i = 0; i < TS_JIT_CACHE_SIZE; i++) {
    if (ctx->jit_cache[i].access_count < min_count) {
      min_count = ctx->jit_cache[i].access_count;
      min_slot = i;
    }
  }
  
  return min_slot;
}
```

---

## 编译流程示例

### 完整编译流程
```c
int turbo_script_compile_mir_jit(turbo_script_ctx_t *ctx, 
                                 const char *script) {
  // 1. 检查 JIT 缓存
  void *cached_fn = ts_mir_cache_lookup(ctx, script);
  if (cached_fn) {
    ctx->mir_last_fn = cached_fn;
    return 0;
  }
  
  // 2. 解析 AST
  exprtk_node_t *ast = turbo_script_parse_with_error(ctx, script);
  if (!ast) return -1;
  
  // 3. 初始化编译器
  ts_mir_compiler_t compiler;
  ts_mir_compiler_init(&compiler, ctx, ast);
  
  // 4. AST Lowering
  MIR_reg_t result_reg = ts_mir_lower_node(&compiler, ast);
  
  // 5. 优化路径应用
  ts_mir_apply_optimizations(&compiler);
  
  // 6. 生成 MIR 模块
  MIR_finish_func(&compiler.ctx);
  MIR_finish_module(&compiler.ctx);
  
  // 7. 加载并链接
  MIR_load_module(compiler.ctx, compiler.module);
  MIR_link(compiler.ctx, MIR_set_gen_interface, NULL);
  
  // 8. JIT 编译为原生代码
  MIR_gen_init(compiler.ctx, 1);
  void *fn_ptr = MIR_gen(compiler.ctx, 0, compiler.func);
  MIR_gen_finish(compiler.ctx);
  
  // 9. 存入缓存
  ts_mir_cache_insert(ctx, script, fn_ptr);
  ctx->mir_last_fn = fn_ptr;
  
  ts_mir_compiler_free(&compiler);
  return 0;
}
```

---

## 调试与诊断

### 启用调试日志
```bash
# 变量同步日志
export TS_DEBUG_SYNC=1

# JIT 缓存日志
export TS_DEBUG_CACHE=1

# JIT 统计
export TS_JIT_STATS=1
```

### 查看 JIT 统计
```c
turbo_script_enable_jit_stats(ctx, 1);

// ... 执行脚本 ...

turbo_script_print_jit_stats(ctx, stderr);
// 输出：
// JIT Statistics:
//   Compile count: 10
//   Exec count: 1000
//   Cache hit rate: 99.0%
//   Avg compile time: 336 μs
//   Avg exec time: 12 μs
```

### GDB 调试
```bash
gdb --args ./your_program

# 断点在编译失败处
(gdb) b ts_mir_fail

# 断点在 lowering 入口
(gdb) b ts_mir_lower_node

# 查看编译器状态
(gdb) p *compiler
```

---

## 性能基准测试

```bash
# 快速基准测试（~10秒）
./bench_turbo_script_mir --quick

# 完整基准测试（~5分钟）
./bench_turbo_script_mir --full

# 单项测试
./bench_turbo_script_mir --test numeric_loop
```

---

## 常见问题

### Q: 为什么有些脚本 JIT 很慢？
A: 可能触发了回退逻辑：
- 嵌套闭包 → 回退到 runtime hook
- 动态类型混合 → 无法应用数值优化
- 大量字符串/Map 操作 → runtime helper 开销

**诊断方法**：
```bash
TS_JIT_STATS=1 ./your_program
# 查看 closure_fallback_count, dynamic_fallback_count
```

### Q: 如何提升 JIT 性能？
A: 优化脚本结构：
1. **数值密集代码**：使用 `number` 类型，避免类型混合
2. **避免嵌套闭包**：改用类或数据结构
3. **预绑定向量**：使用 `ts_bind_vec()` 绑定 double[] 数组
4. **缓存对象字段**：重复访问的字段会触发 slot cache

### Q: JIT 缓存何时失效？
A: 以下情况会触发缓存清空：
- 上下文销毁（`turbo_script_free()`）
- 手动清空（暂未提供 API）
- 缓存已满触发 LRU 淘汰

---

## 参考资料

- [TurboScript Architecture](../../docs/ARCHITECTURE.md)
- [JIT Compilation Guide](../../docs/jit-guide.md)
- [MIR Official Documentation](https://github.com/vnmakarov/mir)
- [Closure Performance Guide](../../docs/CLOSURE_GUIDE.md)（Phase 2.1 交付）

---

## 贡献指南

### 添加新优化路径
1. 在 `mir_optimize.c` 实现识别逻辑
2. 生成优化后的 MIR 指令
3. 在 `test/test_turbo_script_mir.c` 添加测试
4. 在 `bench_turbo_script_mir.c` 添加性能基准

### 添加新 Runtime Helper
1. 在 `mir_runtime.c` 定义 C 函数
2. 在 `ts_mir_externals_t` 添加导入缓存
3. 在初始化阶段注册导入
4. 在 lowering 中调用 helper

### 代码规范
- 函数前缀：`ts_mir_*`
- 静态函数：`static` 且命名以 `_` 开头（如 `_lower_numeric_expr`）
- 错误处理：使用 `ts_mir_fail()` 报告编译错误
- 内存管理：MIR 对象由 MIR 上下文管理，手动分配需配对 `free()`
