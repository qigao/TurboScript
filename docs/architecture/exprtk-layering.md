# ExprTk 分层决策

## 背景

当前 `exprtk` 静态库同时包含语法生成、AST、解释运行时、值与环境、模块注册和动态插件加载；TurboScript 则已拥有独立的 MIR lowering、JIT cache 与执行后端。该布局使语法前端无法独立复用，并让 ExprTk 承担宿主运行时职责。

## 决策

保留 `ExprTk::Core` 作为现有 `exprtk` target 的静态库名称，并新增静态 `ExprTk::Syntax`：

- `ExprTk::Syntax`：re2c lexer、Lemon parser、AST 构造和 `exprtk_parse*`/`exprtk_free`；其公开 AST 值类型暴露 Salts 的 `datetime_t`，因此公开依赖 `Salts::DateTimeParser` 作为已安装头和实现的所有者。
- `ExprTk::Core`：值、环境、解释执行、类、集合、内建模块和正则；它公开依赖 Syntax。
- `TurboScript`：脚本生命周期、`import()`、动态插件加载、任务/计时器，以及全部 MIR/JIT 编译、缓存和执行。

`ExprTk::Syntax` 不依赖 Core 或 TurboScript；Core 不依赖 TurboScript 或 MIR（变量、方法和字段表统一使用 `Salts::CSTL`）；TurboScript 依赖 Core。所有库保持静态，现有 C API 名称不变。

## 取舍

不把 JIT 移到 ExprTk：MIR 代码需要 TurboScript 的上下文、脚本缓存、host ABI 与环境同步桥接。移动它会使通用解释器依赖宿主生命周期，且会引入第二个代码生成事实源。

动态插件加载迁出 Core：插件的路径搜索、ABI 校验和实例生命周期是 TurboScript 的宿主策略，不能成为语法或解释器的依赖。

## 验证

- Syntax 单测只链接 `ExprTk::Syntax`，验证 lexer/parser/AST 与错误位置。
- Core 单测链接 `ExprTk::Core`，验证解释执行、值、环境与模块注册。
- TurboScript host plugin-loader 测试链接其私有 loader 组件，验证动态模块发现、ABI 拒绝和加载生命周期；最终 TurboScript 库仍链接该组件。
- Windows 使用 `win-dev-user` preset，先构建三个库和上述测试，再运行相关 CTest 过滤器。
