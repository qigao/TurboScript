# TurboScript 文档索引

## 核心文档

- [语言指南](./language-guide.md)
- [语法参考](./grammar.md)
- [API 参考](./api/api-reference.md)
- [架构说明](./ARCHITECTURE.md)
- [开发者架构](./advanced/architecture.md)
- [模块说明](./modules/README.md)

## 结构化文档映射

TurboScript 使用 `mapper` 进行 class-first JSON/YAML/XML 映射。class 字段
声明是唯一类型来源：

```turboscript
import("mapper");

class User {
    name: string;
    age: int64;
}

var user = mapper.read_json(User, "{\"name\":\"Ada\",\"age\":37}");
var json = mapper.write_json(user);
```

详见 [Mapper 模块](../modules/mapper/README.md)。文档解析由 Salts
Salts DataBind parser APIs 提供；TurboScript 不再提供独立 schema、codec 或二进制
DataBind 公共接口。

## 其他模块

- `parser`: 配置文本解析
- `os`: 进程、服务、日志和系统电源操作
- `cron`: 定时任务
- `ta` / `fin` / `strategy`: 时间序列、金融与策略辅助
- `vec`: 向量操作
- `net` / `http` / `sqlite`: 网络、HTTP 和数据库能力

## 构建与测试

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user
```
