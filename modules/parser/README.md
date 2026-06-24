# Parser 模块

基于 TurboNet::Parser 的 CSV/JSON/XML/Datetime 解析模块。

## 功能概述

- **CSV 解析**: 支持内存和文件解析，支持自定义分隔符和引号字符
- **JSON 解析**: 查询 JSON 对象字段，或在 JSON 文本与 TurboScript map/list/scalar/null 之间转换
- **XML 解析**: 使用 XPath 1.0 查询 XML，并转换为 TurboScript 原生 list/map
- **Datetime 解析**: 复用 TurboNet datetime parser，解析常见日期时间格式并格式化 RFC822/HTTP 时间
- **TBE Schema 绑定**: 将 JSON/CSV 绑定为 TurboScript map/list，并支持按 schema emit/validate
- **Schema 反射**: 查询 TBE schema 的类型、字段、枚举、属性和布局信息
- **句柄管理**: 最多支持 16 个并发打开的 CSV 文档

## API 文档

### CSV 函数

#### `parser.csv_parse(data: string, has_header: int = 1) -> int`
解析 CSV 字符串数据，返回文档句柄。

**参数**:
- `data`: CSV 数据字符串
- `has_header`: 是否有表头行（默认 1）

**返回**: 文档句柄（>=0 成功，-1 失败）

**示例**:
```javascript
let csv_data = "name,age\nAlice,30\nBob,25";
let handle = parser.csv_parse(csv_data, 1);
```

#### `parser.csv_parse_file(path: string, has_header: int = 1) -> int`
从文件解析 CSV，返回文档句柄。

**参数**:
- `path`: CSV 文件路径
- `has_header`: 是否有表头行（默认 1）

**返回**: 文档句柄（>=0 成功，-1 失败）

**示例**:
```javascript
let handle = parser.csv_parse_file("data.csv", 1);
```

#### `parser.csv_rows(handle: int) -> int`
获取 CSV 文档的行数（不包括表头）。

**参数**:
- `handle`: CSV 文档句柄

**返回**: 数据行数

#### `parser.csv_cols(handle: int) -> int`
获取 CSV 文档的列数。

**参数**:
- `handle`: CSV 文档句柄

**返回**: 列数

#### `parser.csv_get(handle: int, row: int, col: int) -> string`
获取指定单元格的字符串值。

**参数**:
- `handle`: CSV 文档句柄
- `row`: 行索引（从 0 开始，不包括表头）
- `col`: 列索引（从 0 开始）

**返回**: 单元格值（字符串）

**示例**:
```javascript
let name = parser.csv_get(handle, 0, 0);  // 第一行第一列
```

#### `parser.csv_get_num(handle: int, row: int, col: int, default: number = 0) -> number`
获取指定单元格的数值。

**参数**:
- `handle`: CSV 文档句柄
- `row`: 行索引（从 0 开始）
- `col`: 列索引（从 0 开始）
- `default`: 默认值（解析失败时返回）

**返回**: 单元格数值

**示例**:
```javascript
let age = parser.csv_get_num(handle, 0, 1, 0);  // 第一行第二列
```

#### `parser.csv_close(handle: int) -> int`
关闭 CSV 文档，释放资源。

**参数**:
- `handle`: CSV 文档句柄

**返回**: 1（成功）

**示例**:
```javascript
parser.csv_close(handle);
```

#### `csv.filter_table(data: string, expr: string) -> string`
使用 CSV filter expression 过滤 CSV 文本，并返回保留表头的 CSV 文本。该函数面向链式 stream 物化流程；普通脚本需要兼容旧输出时仍可使用 `csv.filter(data, expr)`。

**示例**:
```javascript
let rows = csv.filter_table("price,qty\n10,2\n3,5", "price > 5");
```

### JSON 函数

#### `json.parse(json: string) -> int`
将任意 JSON 文本解析为可复用文档句柄。解析失败返回 `-1`。该句柄可直接用于 `json.bind` / `json.bind_all`，避免每次绑定都重新解析 JSON 文本。

兼容别名：`parser.json_parse(json)`。

#### `json.value(handle: int) -> map|list|string|number|null`
将已解析 JSON 文档句柄转换为 TurboScript 原生值。object 映射为 `map`，array 映射为 `list`，string/number/bool/null 映射为对应脚本值。无效句柄返回 `null`。

#### `json.close(handle: int) -> number`
释放 `json.parse` 返回的 JSON 文档句柄。

**示例**:
```javascript
let json_id = json.parse("{\"name\":\"Alice\",\"scores\":[10,20]}");
let data = json.value(json_id);
let total = data.scores[0] + data.scores[1];
json.close(json_id);
```

#### `json.stringify(value: any) -> string`
将 TurboScript 原生 scalar、`map`、`list` 或 `vector` 序列化为 JSON 文本。无法表示的运行时对象（如 class、instance、function）会输出为 `null`。

兼容别名：`parser.json_stringify(value)`。

**示例**:
```javascript
let text = json.stringify(map{name:"Bob", age:25, tags:list("a", "b")});
let json_id = json.parse(text);
let row = json.value(json_id);
json.close(json_id);
```

### Datetime 函数

#### `datetime.parse(text: string) -> map|null`
解析 RFC-822/HTTP、ISO-8601、NCSA 和常见分隔符日期时间格式，成功返回字段 map，失败返回 `null`。

返回字段包括：`year`、`month`、`day`、`hour`、`minute`、`second`、`millisecond`、`tz_offset`、`has_tz`、`day_of_week`，可转换时还包含 `timestamp`。

兼容别名：`parser.datetime_parse(text)`。

#### `datetime.to_time(value: string|map) -> number`
将 datetime 字符串或 `datetime.parse()` 返回的 map 转为 Unix epoch 秒。失败返回 `-1`。

兼容别名：`datetime.timestamp(value)`、`parser.datetime_to_time(value)`。

#### `datetime.format_rfc822(timestamp: number) -> string`
将 Unix epoch 秒格式化为 RFC822/HTTP GMT 时间字符串，失败返回空字符串。

兼容别名：`parser.datetime_format_rfc822(timestamp)`。

**示例**:
```javascript
let dt = datetime.parse("Sat, 04 Mar 2006 13:27:54 GMT");
let ts = datetime.to_time(dt);
let http_date = datetime.format_rfc822(ts);
```

#### `parser.json_query(json: string, jsonpath: string) -> any`
使用 TurboNet JSONPath 查询 JSON。支持字段、数组下标、通配符、union 和过滤表达式，例如 `$.score`、`$.user.name`、`$.orders[*].qty`、`$.orders[@.price > 5]`。

**参数**:
- `json`: JSON 字符串
- `jsonpath`: JSONPath 查询表达式

**返回**: 无匹配时返回 `null`，单个匹配返回对应原生值，多个匹配返回 list。

**示例**:
```javascript
let json = "{\"user\":{\"name\":\"alice\"},\"score\":99}";
let user = parser.json_query(json, "$.user.name");  // "alice"
```

#### `parser.json_query_num(json: string, jsonpath: string, default: number = 0) -> number`
查询第一个 JSONPath 匹配的数值字段。

**参数**:
- `json`: JSON 字符串
- `jsonpath`: JSONPath 查询表达式
- `default`: 默认值（字段不存在或非数值时返回）

**返回**: 字段数值

**示例**:
```javascript
let qty = parser.json_query_num(json, "$.orders[0].qty", 0);
```

### XML 函数

XML API 通过 TurboNet::Parser 的 opaque XPath node 接口实现，脚本层和外部模块不需要包含 cxml 头文件。

#### `xml.query(xml: string, xpath: string) -> list<map>`
使用 XPath 1.0 查询 XML，返回节点列表。每个节点是 map，字段包括：

- `type`: 节点类型，例如 `element`、`attribute`、`text`
- `name`: 节点名，无名称时为空字符串
- `text`: 节点文本值，无文本时为空字符串
- `xml`: 节点序列化文本

兼容别名：`parser.xml_query(xml, xpath)`。

#### `xml.text(xml: string, xpath: string) -> string`
返回第一个 XPath 命中节点的文本值。未命中或解析失败返回空字符串。

兼容别名：`parser.xml_text(xml, xpath)`。

#### `xml.count(xml: string, xpath: string) -> number`
返回 XPath 命中节点数量。

兼容别名：`parser.xml_count(xml, xpath)`。

**示例**:
```javascript
let doc = "<orders><order id=\"a\"><price>10</price></order></orders>";
let prices = xml.query(doc, "//price");
let id = xml.query(doc, "//@id")[0].text;
let total = xml.count(doc, "//order");
```

### TBE Schema 函数

#### `schema.parse(schema: string) -> int`
解析 TBE schema 并返回模块持有的 schema 句柄。使用完毕后调用 `schema.close(handle)`。

#### `schema.parse_ex(schema: string) -> map`
解析 TBE schema 并返回诊断 map：

```javascript
let r = schema.parse_ex("message Trade { uint32 qty; }");
// { ok, handle, line, column, code, message }
```

#### `schema.error() -> string`
返回最近一次 schema 解析错误消息。

#### `schema.types(schema_or_handle) -> list<map>`
返回 message、composite、group、enum、flags、union 的类型列表。

#### `schema.fields(schema_or_handle, type: string) -> list<map>`
返回指定 record 或 union 的字段列表。字段包含 `name`、`type`、`kind`、`inner_type`、`group_type`、`value_type`、`collection_kind`、`length` 等可用元信息。

#### `schema.enums(schema_or_handle) -> list<map>`
返回 enum/flags 列表，包含名称、underlying type、items、`is_flags` 和 attributes。

#### `schema.flags(schema_or_handle) -> list<map>`
返回 flags 列表，包含名称、underlying type、items 和 attributes。

#### `schema.unions(schema_or_handle) -> list<map>`
返回 union 列表，包含名称、variant count、variants 和 attributes。

#### `schema.attributes(schema_or_handle, type: string = "") -> map`
返回 schema 或指定 type 的 attributes。省略 `type` 时返回 schema declaration 的 attributes。

#### `schema.layout(schema_or_handle, type: string = "") -> map`
返回 schema 或指定 type 的布局摘要，例如 `kind`、`fixed_block_size`、`field_count`、`wire_byte_order` 和 attributes。

### Schema Binding 函数

JSON:

- `json.bind(schema_handle, json_handle, type) -> map`
- `json.bind_all(schema_handle, json_handle, type) -> list<map>`
- `json.bind_schema(schema_handle, json_handle, type) -> map`
- `json.bind_all_schema(schema_handle, json_handle, type) -> list<map>`
- `json.emit(schema, value, type) -> string`
- `json.emit_schema(schema_handle, value, type) -> string`
- `json.validate(schema, json, type) -> int`
- `json.validate_schema(schema_handle, json, type) -> int`
- `json.validate_ex(schema, json, type) -> map`
- `json.validate_ex_schema(schema_handle, json, type) -> map`

JSON binding 的 `type` 可以是 record、union、scalar、enum 或 flags。顶层 scalar/enum/flags 与字段内语义一致，`json.bind_all` 可绑定顶层 scalar/enum/flags 数组。

绑定失败语义：

- 非法标量、未知 enum 项或未知 flags 项不会被静默转换为 `0`
- record 字段存在但不符合 schema 时，整条 record 绑定失败
- 必填字段缺失时，整条 record 绑定失败
- `bind_all` 会跳过无法绑定的元素；`validate` / `validate_ex` 用于判断原始输入整体是否全部合法并返回诊断
- fixed array 长度必须匹配 schema；JSON list/group 必须是 array，map 必须是 object

CSV:

- `csv.bind(schema_handle, csv_handle, row, type) -> map`
- `csv.bind_all(schema_handle, csv_handle, type) -> list<map>`
- `csv.bind_schema(schema_handle, csv_handle, row, type) -> map`
- `csv.bind_all_schema(schema_handle, csv_handle, type) -> list<map>`
- `csv.emit(schema, value, type) -> string`
- `csv.emit_schema(schema_handle, value, type) -> string`
- `csv.validate(schema, csv, type) -> int`
- `csv.validate_schema(schema_handle, csv, type) -> int`
- `csv.validate_ex(schema, csv, type) -> map`
- `csv.validate_ex_schema(schema_handle, csv, type) -> map`

CSV binding 以行对象为入口，`type` 可以是 message/composite/group 这类 record、union、scalar、enum 或 flags；record 字段内可继续使用 enum、flags、集合、map 和嵌套结构。顶层 scalar/enum/flags 使用 `value` 列作为标准列名，bind/validate 也兼容单列 CSV 的第一列。CSV 绑定失败语义与 JSON 一致：非法标量、未知 enum/flags 项、必填字段缺失或字段值不符合 schema 时不会生成伪有效值，`bind_all` 跳过无法绑定的行。

支持的 TBE 字段形状：

- scalar: `uint32 qty;`
- composite: `Header header;`
- fixed array: `uint32[2] levels;`
- dynamic `list<T>` / `set<T>`
- `group<T>`
- `map<string,T>`
- union 字段

`optional` 字段在 JSON/CSV validate 中允许缺失；带 `default` 的 scalar 字段在 bind 时会补入默认值。单纯 optional 字段缺失时不会写入结果 map。

enum 字段接受数字值或 item 名称；flags 字段接受数字 bitmask、`Read|Write` / `Read,Write` 形式的分隔字符串，JSON flags 还接受名称或数字数组。`json.emit` / `csv.emit` 仍输出数字值。

JSON union 使用 canonical 单 variant object 映射：

```json
{ "success": { "code": 200 } }
```

对象必须只包含一个 union variant key；空对象、多个 variant、未知 variant 都会被 validate 拒绝。variant payload 继续按其 TBE type 做 bind、emit 和 validate。

CSV 使用扁平列名表示嵌套结构，例如 `header.seq`、`levels[0]`、`bids[0].price`、`attrs.x`。union 字段使用 `choice.side`、`choice.success.code` 这样的 variant path，顶层 union 使用无外层前缀的 variant 列名，例如 `side`、`success.code`，容器内 union 使用 `choices[0].side`、`by_key.a.perms`；每个 union 值必须刚好提供一个 variant。顶层 scalar/enum/flags 的 `csv.emit` 输出 `value` 表头。列名同时兼容 sanitized 形式：`header_seq`、`levels_0`、`bids_0_price`、`attrs_x`。

`csv.emit` 输出多行 record 时会合并所有行实际出现的动态列，例如不同 group 长度、map key 或 union variant；空占位单元在 `csv.bind_all` / `csv.validate` 中按该行未提供对应动态项处理。

Schema binding 当前主线已覆盖：

- JSON/CSV `bind`、`bind_all`、`emit`、`validate`、`validate_ex`
- schema text 与 schema handle 两种入口
- record/composite/group、fixed array、list、set、map
- scalar、enum、flags
- union 顶层值、record 字段、容器元素、scalar payload 和 record payload
- optional/default 字段

## 完整示例

### CSV 解析示例

```javascript
import("parser");

// 解析 CSV 字符串
let csv_data = "name,age,city\nAlice,30,NYC\nBob,25,LA";
let handle = parser.csv_parse(csv_data, 1);

if (handle >= 0) {
    let rows = parser.csv_rows(handle);
    let cols = parser.csv_cols(handle);
    print("Rows:", rows, "Cols:", cols);
    
    // 遍历所有行
    for (let i = 0; i < rows; i = i + 1) {
        let name = parser.csv_get(handle, i, 0);
        let age = parser.csv_get_num(handle, i, 1, 0);
        let city = parser.csv_get(handle, i, 2);
        print(name, age, city);
    }
    
    parser.csv_close(handle);
}
```

### JSON 查询示例

```javascript
import("parser");

let json = "{\"user\":\"alice\",\"score\":99,\"active\":true}";

let user = parser.json_query(json, "$.user");
let score = parser.json_query_num(json, "$.score", 0);
let active = parser.json_query_num(json, "$.active", 0);

print("User:", user);
print("Score:", score);
print("Active:", active);
```

### JSON 原生映射示例

```javascript
import("parser");

let json_id = json.parse("{\"name\":\"Alice\",\"orders\":[{\"qty\":2},{\"qty\":3}]}");
let data = json.value(json_id);
let qty = data.orders[0].qty + data.orders[1].qty;

let out = json.stringify(map{name:data.name, qty:qty});
json.close(json_id);
```

### TBE Schema Binding 示例

```javascript
import("parser");

let schema_text = "group Level { uint64 price; uint32 qty; } "
                + "message Book { uint32 seq; group<Level> bids; map<string,int32> attrs; }";

let schema_id = schema.parse(schema_text);
let csv_data = "seq,bids[0].price,bids[0].qty,attrs.x\n7,100,10,3";
let csv_id = parser.csv_parse(csv_data);

let book = csv.bind_schema(schema_id, csv_id, 0, "Book");
let ok = csv.validate_schema(schema_id, csv_data, "Book");
let json_text = json.emit_schema(schema_id, book, "Book");

parser.csv_close(csv_id);
schema.close(schema_id);
```

## 注意事项

1. **句柄限制**: 最多同时打开 16 个 CSV 文档、16 个 JSON 文档和 16 个 schema
2. **行索引**: 当 `has_header=1` 时，行索引从 0 开始（第一个数据行）
3. **资源释放**: 使用完毕后务必调用 `parser.csv_close()`、`json.close()` 或 `schema.close()` 释放资源
4. **错误处理**: 函数失败时返回 0 或空字符串，应检查返回值
5. **内存分配**: 字符串值使用 scratch arena 分配，在表达式求值结束后自动释放

## 构建和测试

### 构建模块

```bash
cmake --build build --target parser
cmake --build build --target parser_plugin
```

### 运行 C 单元测试

```bash
cd build
ctest -R parser_module -V
```

### 运行 TurboScript 测试

```bash
.\build\Msvc\bin\turbo_script_repl.exe -f test_parser.ts
.\build\Msvc\bin\turbo_script_repl.exe -f test_parser_file.ts
```

## 实现细节

- **静态库**: `parser` - 用于单元测试
- **动态库**: `parser_plugin.dll` - 用于 TurboScript 运行时加载
- **依赖项**: 
  - `exprtk` - TurboScript 核心
  - `TurboNet::Utils` - 内存池和字符串工具
  - `TurboNet::Parser` - CSV/JSON/XML/Datetime 解析引擎

## 参考

- [TurboNet::Parser 文档](https://github.com/your-org/turbonet)
- [TurboScript 插件开发指南](../../docs/PLUGIN_SYSTEM.md)
- [模块开发规范](../README.md)
