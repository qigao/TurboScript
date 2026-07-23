# TurboScript

**一个 TypeScript-like 语法的高性能嵌入式脚本引擎，基于统一 MIR 后端，同时支持 MIR 解释执行、MIR JIT 和可扩展插件系统。**

TurboScript 面向需要内嵌脚本能力的宿主应用，而不是完整替代 JavaScript、Python 或 Lua 运行时。它提供接近 TypeScript/JavaScript 的语法，`turbo_script_run()` 与 `turbo_script_run_jit()` 共享同一套 MIR lowering，并提供数据绑定、文本处理、数学、矩阵/线性代数、时间序列、技术分析和金融计算模块。

它不是完整 TypeScript 实现，也不是 Pandas/NumPy 的完整替代品。当前定位是嵌入式自动化、数据转换、轻量数据分析、量化指标计算和宿主应用扩展。

---

## 🚀 快速链接

### 用户文档
- **[快速开始](getting-started.md)** - 5 分钟教程，编写你的第一个脚本
- **[语言指南](language-guide.md)** - 完整的语法参考和语言特性
- **[语法特性分析](syntax-features-analysis.md)** - 当前语法能力、OOP、JIT/解释器边界和缺口
- **[API 参考](api-reference.md)** - 内置函数和标准库

### 开发者文档
- **[插件开发](plugin-development.md)** - 使用 C/C++ 扩展 TurboScript
- **[模块文档](../modules/)** - 领域特定模块指南
- **[架构设计](../advanced/architecture.md)** - 内部设计和实现

### 速查表
- **[数学函数](../math_cheatsheet.md)** - 数学函数速查
- **[向量操作](../vec_cheatsheet.md)** - 向量/数组操作
- **[技术分析](../ta_fin_cheatsheet.md)** - TA 指标和金融函数

---

## ✨ 核心特性

### 现代化语言设计
- **TypeScript/JavaScript-like 语法**，适合嵌入式脚本编写
- **动态类型**，支持类型内省（`typeof`、`is_number` 等）
- **箭头函数**和闭包：`(x) => x * 2`
- **解构赋值**：`let [a, b] = [10, 20]`
- **管道操作符**：`data |> filter(x > 0) |> sum()`
- **可选链**：`user?.address?.city`
- **类和接口**：`class` / `interface` / `extends` / `implements` / `super`

### 丰富的数据类型
- **数字**：64 位浮点数
- **字符串**：UTF-8，支持模板字符串
- **向量**：高效的数值数组
- **对象**：parser API 返回的 plain record，class instance 表达强类型对象
- **映射**：基于哈希表的键值存储
- **列表**：异构集合

### 内置库
- **数学与矩阵**：标量数学、统计、矩阵辅助函数、线性代数
- **字符串**：UTF-8 文本操作、解析、格式化、模板
- **Parser/Mapper**：配置文本解析，以及基于 class 的 JSON/YAML/XML 映射
- **时间序列 / TA / 金融**：rolling/window、技术指标、风险指标、组合辅助函数
- **文件 I/O**：读写文件、目录操作
- **日期/时间**：解析、格式化、时间戳

### 可扩展架构
- **插件系统**：通过 `import("plugin_name")` 动态加载 C/C++ DLL
- **模块化**：清晰的命名空间分离（`csv.*`、`json.*`、`ta.*`、`strategy.*`）
- **统一 MIR 后端**：`turbo_script_run()` 执行生成后的 MIR 解释模式，`turbo_script_run_jit()` 使用同一份 IR 生成原生代码
- **纯 MIR 执行契约**：未覆盖的 lowering 形式由 MIR pipeline 显式报错
- **热路径优化**：直接数学调用、预绑定向量访问、map 数值指针、单态 OOP 方法 lowering、`super.method(...)` inline、public numeric instance field slot 访问
- **零拷贝 FFI**：与宿主应用程序高效数据交换

---

## 📦 安装

### 从源码构建
```bash
git clone https://github.com/your-org/turbonet.git
cd turbonet/tScript
mkdir build && cd build
cmake ..
make
```

### 使用预编译二进制
从 [Releases](https://github.com/your-org/turbonet/releases) 下载最新版本。

---

## 🎯 快速示例

```javascript
// 加载插件
import("csv");
import("ta");

// 读取和处理数据
var data = csv.read("prices.csv");
var close = csv.col(data, "close");

// 计算技术指标
var sma20 = ta.sma(close, 20);
var rsi14 = ta.rsi(close, 14);

// 生成信号
var signal = (rsi14 < 30) ? "买入" : (rsi14 > 70) ? "卖出" : "持有";

print("信号: " + signal);
```

---

## 🌍 语言支持

- **English**: [Primary documentation](../README.md)
- **中文**: 主文档（当前目录）

---

## 📚 模块生态系统

TurboScript 提供了丰富的可选模块：

| 模块 | 说明 | 文档 |
|--------|-------------|---------------|
| `parser` | 面向配置的文本解析（INI、dotenv、TOML、命令行） | [parser README](../../modules/parser/README.md) |
| `mapper` | 基于 class 的 JSON/YAML/XML 映射 | [mapper README](../../modules/mapper/README.md) |
| `ta` | 技术分析指标 | [ta_fin_cheatsheet.md](../ta_fin_cheatsheet.md) |
| `fin` / `strategy` | 风险指标、策略上下文、组合辅助函数 | [ta_fin_cheatsheet.md](../ta_fin_cheatsheet.md) |
| `vec` | 高级向量操作 | [vec_cheatsheet.md](../vec_cheatsheet.md) |
| `net` | HTTP/WebSocket 网络 | [modules/net.md](../modules/net.md) |
| `sqlite` | 数据库访问 | [modules/sqlite.md](../modules/sqlite.md) |
| `rules_forge` | 规则引擎集成、外部 RulesForge fact、TurboScript RHS 插件加载 | [rules_forge README](../../modules/rules_forge/README.md) |
| `os` | 平台信息、无 shell 子进程、日志、服务和电源管理 | [os README](../../modules/os/README.md) |
| `wasm` | WebAssembly 执行 | [../../modules/wasm/README.md](../../modules/wasm/README.md) |

---

## 🛠️ 开发

### 项目结构
```
tScript/
├── exprtk/          # 核心解析器、解释器、内置模块
├── turbo_script/    # 宿主 API、MIR JIT、脚本运行时
├── modules/         # 内置和插件模块
├── docs/            # 文档（你在这里）
└── tests/           # 测试套件
```

### 贡献
参见 [CONTRIBUTING.md](../../CONTRIBUTING.md) 了解贡献指南。

---

## 📄 许可证

[Your License Here]

---

## 🤝 社区

- **问题反馈**：[GitHub Issues](https://github.com/your-org/turbonet/issues)
- **讨论**：[GitHub Discussions](https://github.com/your-org/turbonet/discussions)
- **Discord**：[加入我们的服务器](https://discord.gg/your-invite)

---

**为高性能脚本而生 ❤️**
