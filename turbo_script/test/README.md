# TurboScript 测试套件

本目录包含 TurboScript 的完整测试套件，包括单元测试、集成测试、性能基准测试和 JIT 测试。

---

## 测试文件

| 文件 | 描述 | 类型 |
|------|------|------|
| `test_turbo_script_basics.c` | 基础语言与运行时测试 | 集成测试 |
| `test_turbo_script_quant.c` | 量化相关内置能力测试 | 集成测试 |
| `test_turbo_script_scientific.c` | 科学计算、模板与 regex 测试 | 集成测试 |
| `test_turbo_script_io.c` | IO 与插件相关测试 | 集成测试 |
| `test_turbo_script_mir_core.c` | JIT 核心路径测试 | 单元测试 |
| `test_turbo_script_mir_oop.c` | JIT OOP 路径测试 | 单元测试 |
| `test_turbo_script_mir_advanced.c` | JIT 高级语义测试 | 单元测试 |
| `bench_turbo_script.c` | 解释器性能基准 | 性能测试 |
| `bench_turbo_script_mir.c` | JIT 性能基准 | 性能测试 |

---

## 构建测试

### 使用 CMake

```bash
cd build
cmake ..
cmake --build . --target test_turbo_script_scientific
cmake --build . --target test_turbo_script_mir_core
cmake --build . --target bench_turbo_script_mir
```

### 使用 Make (Linux/macOS)

```bash
cd build
make test_turbo_script_scientific
make test_turbo_script_mir_core
make bench_turbo_script_mir
```

### 使用 MSBuild (Windows)

```powershell
cd build
msbuild turbo_script.sln /t:test_turbo_script_scientific
msbuild turbo_script.sln /t:test_turbo_script_mir_core
msbuild turbo_script.sln /t:bench_turbo_script_mir
```

---

## 运行测试

### 运行所有测试

```bash
cd build
ctest -V
```

### 运行特定测试

```bash
# JIT 编译器测试
ctest -R "test_turbo_script_mir_(core|oop|advanced)" -V

# 解释器拆分测试
ctest -R "test_turbo_script_(basics|quant|scientific|io)" -V

# regex 相关测试
ctest -R test_turbo_script_scientific -V
```

### 直接运行可执行文件

```bash
cd build/bin

# JIT 测试
./test_turbo_script_mir_core
./test_turbo_script_mir_oop
./test_turbo_script_mir_advanced

# 性能基准
./bench_turbo_script_mir
```

---

## 性能基准测试

### 运行 JIT 性能基准

```bash
cd build/bin
./bench_turbo_script_mir
```

**输出示例**：
```
TurboScript JIT Performance Benchmarks
======================================

=== Pure Loop (100k iterations) ===
Script: sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }
Iterations: 100 (warmup: 10)

Results:
  JIT:         245.123 ms total, 2.451230 ms/iter
  Interpreter: 3421.567 ms total, 34.215670 ms/iter
  Speedup:     13.96x
  Result:      4999950000.00 (verified)

=== Math Functions (sin, 10k iterations) ===
...
```

### 解读基准结果

- **Speedup < 5x**: 可能使用了不适合 JIT 的脚本，或编译开销过大
- **Speedup 10-15x**: 正常范围，符合预期
- **Speedup > 20x**: 优秀，纯 JIT 路径无回退

## 内存泄漏检测

### 使用 Valgrind (Linux/macOS)

```bash
cd build/bin
valgrind --leak-check=full --show-leak-kinds=all ./test_turbo_script_mir_core
```

**预期输出**：
```
HEAP SUMMARY:
    in use at exit: 0 bytes in 0 blocks
  total heap usage: X allocs, X frees, Y bytes allocated

All heap blocks were freed -- no leaks are possible
```

### 使用 AddressSanitizer (所有平台)

```bash
# 重新编译，启用 ASan
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build .

# 运行测试
./bin/test_turbo_script_mir_core
```

---

## 性能分析

### 使用 perf (Linux)

```bash
cd build/bin
perf record -g ./bench_turbo_script_mir
perf report
```

### 使用 Instruments (macOS)

```bash
cd build/bin
instruments -t "Time Profiler" ./bench_turbo_script_mir
```

### 使用 Visual Studio Profiler (Windows)

1. 在 Visual Studio 中打开解决方案
2. 右键点击 `bench_turbo_script_mir` 项目
3. 选择 "性能分析器" -> "CPU 使用率"
4. 点击 "启动"

---

## 持续集成

### GitHub Actions 示例

```yaml
name: TurboScript Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build
        run: |
          mkdir build && cd build
          cmake ..
          cmake --build .
      - name: Run Tests
        run: |
          cd build
          ctest -V
      - name: Run Benchmarks
        run: |
          cd build/bin
          ./bench_turbo_script_mir
```

---

## 测试覆盖率

### 生成覆盖率报告 (Linux/macOS)

```bash
# 重新编译，启用覆盖率
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build .

# 运行测试
ctest

# 生成报告
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' --output-file coverage.info
lcov --list coverage.info
genhtml coverage.info --output-directory coverage_html

# 查看报告
open coverage_html/index.html
```

---

## 故障排查

### 测试失败

**问题**: `test_turbo_script_mir_core` 失败
**解决方案**:
1. 检查 MIR 库是否正确链接：`ldd ./test_turbo_script_mir_core`
2. 检查平台支持：MIR 仅支持 x86_64/aarch64/ppc64le/s390x/riscv64
3. 查看错误日志：`./test_turbo_script_mir_core 2>&1 | tee test.log`

**问题**: 性能基准显示 JIT 比解释器慢
**解决方案**:
1. 检查是否首次运行（包含编译时间）
2. 增加迭代次数以摊销编译开销
3. 检查脚本是否包含 JIT 不支持特性（使用 MIR 拆分测试验证）

**问题**: 内存泄漏
**解决方案**:
1. 使用 Valgrind 定位泄漏点
2. 检查 `turbo_script_free()` 是否正确调用
3. 检查 MIR 上下文是否正确清理

---

## 添加新测试

### 添加 JIT 测试

1. 按范围编辑 `test_turbo_script_mir_core.c`、`test_turbo_script_mir_oop.c` 或 `test_turbo_script_mir_advanced.c`
2. 添加新的 `it()` 块：
   ```c
   it("should handle new feature") {
     check(turbo_script_run_jit(ctx, "x = 42;") == 0);
     check(fabs(ts_get_num(ctx, "x") - 42.0) <= EPS);
   }
   ```
3. 重新编译并运行

### 添加性能基准

1. 编辑 `bench_turbo_script_mir.c`
2. 添加新的 `benchmark_t` 条目：
   ```c
   {.name = "New Benchmark",
    .script = "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }",
    .iterations = 100,
    .warmup = 10},
   ```
3. 重新编译并运行

---

## 参考资料

- [TurboScript JIT 指南](../../docs/jit-guide.md)
- [TurboScript API 参考](../../docs/api-reference.md)
- [MIR 项目](https://github.com/vnmakarov/mir)
- [TinyTest 文档](https://github.com/codeplea/tinytest)

---

**最后更新**: 2026-05-19  
**维护者**: TurboScript 开发团队
