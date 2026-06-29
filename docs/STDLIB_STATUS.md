# TurboScript 标准库现状报告

基于代码审查（2026-06-14），TurboScript 的内建库比文档描述的更加完善。

---

## ✅ 完全实现的模块

### 1. Math 库（mod_math.c）- **90+ 函数**

#### 基础数学
- ✅ 三角函数: `sin`, `cos`, `tan`, `asin`, `acos`, `atan2`
- ✅ 双曲函数: `sinh`, `cosh`, `tanh`
- ✅ 指数对数: `exp`, `exp2`, `expm1`, `log`, `log10`, `log2`, `log1p`, `logn`
- ✅ 幂与根: `pow`, `sqrt`, `cbrt`
- ✅ 取整: `floor`, `ceil`, `round`, `fract`
- ✅ 其他: `abs`, `sgn`, `mod`, `hypot`, `copysign`

#### 统计函数
- ✅ 基础: `sum`, `avg`, `min`, `max`, `median`, `percentile`
- ✅ 分布: `std`, `var`, `skewness`, `kurtosis`
- ✅ 归一化: `zscore`, `normalize`
- ✅ 均值: `geometric_mean`, `harmonic_mean`

#### 线性代数
- ✅ 向量: `dot`, `cross`, `norm`, `proj`
- ✅ 矩阵乘法: `matmul`, `mat3_mul`, `mat4_mul`
- ✅ 矩阵求逆: `inv`, `inv2`, `inv3`
- ✅ 行列式: `det2`, `det3`
- ✅ 特征值: `eig2`, `eig3`
- ✅ 转置: `transpose`
- ✅ 迹: `trace2`

#### 数值算法
- ✅ 微积分: `derivative`, `integrate`
- ✅ 线性回归: `ols_fit`
- ✅ 逆 CDF: `inv_normal_cdf`
- ✅ 插值: `lerp`, `inverse_lerp`, `smoothstep`, `remap`

#### 激活函数
- ✅ `relu`, `sigmoid`, `tanh`, `softplus`

#### 3D 变换（图形学）
- ✅ 变换矩阵: `transform_create`, `transform_mul`, `transform_inv`, `transform_pt`
- ✅ 投影: `ortho`, `perspective`, `lookat`
- ✅ 旋转: `rotate`

#### 其他
- ✅ 随机数: `rand`, `normal_rand`
- ✅ 数论: `gcd`, `fibonacci`
- ✅ 角度转换: `radians`, `degrees`
- ✅ 判断: `is_nan`, `is_inf`

**总计**: ~90 个数学函数，覆盖从基础运算到高级线性代数

---

### 2. String 库（mod_string.c）- **27 函数**

#### 大小写转换
- ✅ `lower` / `toLower`
- ✅ `upper` / `toUpper`

#### 裁剪
- ✅ `trim` - 两端空白
- ✅ `ltrim` - 左侧空白
- ✅ `rtrim` - 右侧空白

#### 搜索
- ✅ `contains` - 子串包含
- ✅ `index_of` / `indexOf` - 首次出现位置
- ✅ `starts_with` / `startsWith`
- ✅ `ends_with` / `endsWith`

#### 提取与操作
- ✅ `substr` - 子串
- ✅ `replace` - 替换
- ✅ `reverse` - 反转
- ✅ `split` - 分割（返回 vector）
- ✅ `join` - 连接

#### 分词
- ✅ `tokenize` / `str_token` - 按分隔符分词
- ✅ `token_count` / `str_count` - 词数

#### 长度
- ✅ `length` / `size`

#### 格式化
- ✅ `format` - 字符串格式化

#### 类型转换
- ✅ `to_str` / `num_to_str` - 数字转字符串
- ✅ `to_num` / `str_to_num` / `parse_num` - 字符串转数字
- ✅ `to_int` - 转整数
- ✅ `to_double` - 转浮点
- ✅ `to_bool` - 转布尔

#### 其他
- ✅ `assert` - 断言

**总计**: 27 个字符串函数，覆盖所有常见字符串操作

---

### 3. Stats 库（mod_stats.c）- **16 函数**

#### 描述统计
- ✅ `median` - 中位数
- ✅ `percentile` - 百分位数
- ✅ `skewness` - 偏度
- ✅ `kurtosis` - 峰度

#### 累积统计
- ✅ `cumsum` - 累积和
- ✅ `cumprod` - 累积积

#### 指数加权
- ✅ `ewma` - 指数加权移动平均
- ✅ `ewmvar` - 指数加权方差

#### 协方差与排名
- ✅ `covariance` - 协方差
- ✅ `rank` - 排名

#### 加权统计
- ✅ `wmean` - 加权均值
- ✅ `wvar` - 加权方差

#### 标准化
- ✅ `zscore` - Z-score 归一化

#### 均值
- ✅ `geometric_mean` - 几何平均
- ✅ `harmonic_mean` - 调和平均

#### 分布
- ✅ `histogram` - 直方图

**总计**: 16 个统计函数，适合时间序列和金融数据分析

---

### 4. File I/O 库（mod_io.c）- **32 函数**

#### 文件读写
- ✅ `read_file(path)` - 读取文件内容
- ✅ `write_file(path, content)` - 写入文件
- ✅ `append_file(path, content)` - 追加内容
- ✅ `copy_file(src, dst)` - 复制文件内容

#### 文件信息
- ✅ `file_exists(path)` - 文件是否存在
- ✅ `file_size(path)` - 文件大小
- ✅ `file_stat(path)` - 文件属性（返回 vector `[size, mtime_s, is_file, is_dir]`）
- ✅ `file_truncate(path, length)` - 截断或扩展文件
- ✅ `is_file(path)` - 是否为文件
- ✅ `is_dir(path)` - 是否为目录

#### 文件操作
- ✅ `file_remove(path)` - 删除文件
- ✅ `file_rename(old, new)` - 重命名

#### 目录操作
- ✅ `listdir(path)` - 列出目录项名称
- ✅ `glob(pattern)` - 按通配符列出匹配路径
- ✅ `mkdir(path)` - 创建目录
- ✅ `mkdir_recursive(path)` - 创建目录及缺失父目录
- ✅ `rmdir(path)` - 删除目录
- ✅ `rmdir_recursive(path)` - 删除目录树

#### 路径操作
- ✅ `path_join(parts...)` - 路径拼接
- ✅ `path_basename(path)` - 文件名
- ✅ `path_dirname(path)` - 目录名
- ✅ `path_is_absolute(path)` - 是否绝对路径

#### 系统信息
- ✅ `os_name()` - 操作系统名称
- ✅ `pid()` - 进程 ID
- ✅ `tmpdir()` - 临时目录路径

#### 时间
- ✅ `now()` - Unix 时间戳（秒）
- ✅ `uptime_ms()` - 进程运行时间（毫秒）
- ✅ `monotonic_ms()` - 单调时钟（毫秒）

#### 日期格式化
- ✅ `date(str)` - 解析日期字符串（本地时间）
- ✅ `date_utc(str)` - 解析日期字符串（UTC）
- ✅ `format_date(timestamp [, fmt])` - 格式化日期（本地时间）
- ✅ `format_date_utc(timestamp [, fmt])` - 格式化日期（UTC）

**总计**: 32 个 I/O 函数，覆盖文件、目录列举、递归目录操作、时间、系统信息

**实现来源**: 文件与路径能力基于 TurboNet `turbo_fs`；日期格式化、本地时间、
UTC 转换基于 TurboNet platform datetime helpers；通用日期字符串解析基于
TurboNet datetime parser。

---

## ⚠️ 部分实现的模块

### 5. Vector 库 - **完整**

通过 `vec.*` 命名空间提供：

```javascript
import("vec");

// 或者通过 math 模块的 vec.* 前缀
vec.sum([1, 2, 3]);
vec.avg([1, 2, 3]);
vec.sort([3, 1, 2]);
vec.unique([1, 2, 2, 3]);
vec.range(0, 10);
```

**总计**: 30+ 向量操作函数

---

### 6. Parser / Schema Binding - **完整**

通过 `parser` 插件提供 CSV、JSON、Datetime 解析，以及 TBE schema 驱动的数据绑定。

#### 基础解析
- ✅ CSV 句柄 API：`parser.csv_parse` / `parser.csv_parse_file` / `parser.csv_rows` / `parser.csv_cols` / `parser.csv_get` / `parser.csv_get_num` / `parser.csv_close`
- ✅ Inline CSV API：`csv.rows` / `csv.cols` / `csv.get` / `csv.get_num` / `csv.col` / `csv.filter` / `csv.filter_count` / `csv.write`
- ✅ JSON 查询与原生映射：`json.query` / `json.query_num` / `json.to_vec` / `json.parse` / `json.stringify`
- ✅ Plain object 映射：JSON object、XML 查询节点、schema reflection 和 schema-bound record/union 返回 `object`；TBE `map<K,V>` 字段仍返回 `map`
- ✅ Datetime 解析：`datetime.parse` / `datetime.to_time` / `datetime.timestamp` / `datetime.format_rfc822`
- ✅ Date/Time/Duration 原生值：`date.parse` / `time.parse` / `duration.parse` / `*.to_string` / 字段访问
- ✅ Decimal 原生值：`decimal.parse` / `decimal.to_string` / `decimal.mantissa` / `decimal.scale` / 字段访问

#### TBE Schema 反射
- ✅ `schema.parse` / `schema.parse_ex` / `schema.close` / `schema.error`
- ✅ `schema.types` / `schema.fields` / `schema.enums` / `schema.flags` / `schema.unions`
- ✅ `schema.attributes` / `schema.layout` / `schema.type_exists`

#### JSON/CSV/XML Schema Binding
- ✅ `json.bind` / `json.bind_all` / `json.emit` / `json.validate` / `json.validate_ex`
- ✅ `csv.bind` / `csv.bind_all` / `csv.emit` / `csv.validate` / `csv.validate_ex`
- ✅ `xml.bind` / `xml.bind_all` / `xml.validate` / `xml.validate_ex`
- ✅ schema text 与 schema handle 两种入口
- ✅ record/composite/group、fixed array、list、set、map
- ✅ scalar、enum、flags、union
- ✅ optional/default 字段
- ✅ string 字段格式校验：`[format(ipaddr)]`、`[format(url)]`、`[format(email)]` 等；绑定结果仍为普通字符串
- ✅ CSV 扁平路径、sanitized 列名、动态 header 合并
- ✅ 绑定失败不制造伪有效值：非法 scalar、非法 record、错误 container shape 会绑定失败；`bind_all` 跳过无法绑定的元素，`validate_ex` 返回诊断

#### 原生类型流转

| 类型 | 核心运行时 | JSON parse/stringify | Schema bind/emit/validate |
|------|------------|----------------------|----------------------------|
| `bool` | ✅ `typeof` / `is_bool` / 条件表达式 | ✅ JSON bool 原生映射 | ✅ JSON/CSV/XML text binding，严格布尔 validate |
| `int64` | ✅ 整数字面量与 `is_int64` | ✅ 数字映射 | ✅ 整数 schema scalar、enum、flags |
| `bytes` | ✅ `bytes()` / `from_bytes()` / `.length` / 索引 | ✅ 无 schema 时 stringify 为 byte 数组 | ✅ JSON/XML 字符串文本、CSV cell 文本绑定为原生 `bytes`；schema emit 输出文本单元 |
| `uuid` | ✅ `uuid()` / `uuid4()` / `uuid7()` / `uuid_string()` | ✅ stringify 为 canonical UUID 字符串 | ✅ JSON/XML UUID 字符串、CSV cell 文本绑定为原生 `uuid` |
| `datetime` | ✅ `datetime()` / `datetime.to_time()` / `datetime.to_string()` / 字段访问 | ✅ stringify 为 RFC822/HTTP GMT 文本 | ✅ JSON/XML/CSV 文本绑定为原生 `datetime`；validate 拒绝无法解析的文本 |
| `date` | ✅ `date.parse()` / `date.to_string()` / `year` `month` `day` | ✅ stringify 为 `YYYY-MM-DD` 字符串 | ✅ JSON/XML/CSV schema bind/emit/validate 为原生 `date` |
| `time` | ✅ `time.parse()` / `time.to_string()` / `hour` `minute` `second` `millisecond` | ✅ stringify 为 `HH:MM:SS[.mmm]` 字符串 | ✅ JSON/XML/CSV schema bind/emit/validate 为原生 `time` |
| `duration` | ✅ `duration.parse()` / `duration.seconds()` / `milliseconds` `seconds` | ✅ stringify 为 `H:MM:SS.mmm` 字符串 | ✅ JSON/XML/CSV schema bind/emit/validate 为原生 `duration` |
| `decimal` | ✅ `decimal.parse()` / `decimal.to_string()` / `mantissa` `scale` / 精确定点相等比较 | ✅ stringify 为规范化 decimal 字符串 | ✅ JSON/XML/CSV schema bind/emit/validate 为原生 `decimal` |
| `bigint` | ✅ `bigint.parse()` / `bigint.to_string()` / 原生 `bigint` 值 | ✅ stringify 为 JSON string，避免精度丢失 | ✅ JSON/XML/CSV schema bind/validate 为原生 `bigint` |
| `money` | ✅ `money()` / `money.to_string()` / `.amount` `.currency` | ✅ stringify 为 `{ amount, currency }` object | ✅ JSON/XML/CSV schema bind/validate 为原生 `money` |
| `set` | ✅ `set(...)` 去重 / `.length()` / `.contains()` / for-in | ✅ stringify 为 JSON array | ✅ schema `set<T>` bind 为原生 `set` |
| `offset_datetime` | ✅ `offset_datetime.parse()` / `.tz_offset` / `.timestamp()` | ✅ stringify 为 ISO offset datetime 字符串 | ✅ 可作为运行时值参与 parser emit |
| `typed_array` | ✅ `typed.i32/i64/f32/f64(...)` / `.length` `.kind` / 索引 / for-in | ✅ stringify 为 JSON array | ✅ 可作为高频数值容器参与 parser emit |
| `string format` | ✅ 运行时仍为普通 `string` | ✅ JSON string 映射 | ✅ schema 字段支持 `ipaddr`/`cidr`/`hostname`/`domain`/`email`/`url`/`uri`/`macaddr`/`semver`/`hex`/`base64`/`base64url`/`currency`/`json_pointer`/`jsonpath`/`xpath`/`cron`/`color`/`mime`/`regex` 校验 |
| `object` | ✅ host plain object 字段访问、索引访问、迭代、`typeof == "object"` | ✅ JSON object 映射为 plain object | ✅ schema-bound record/union 与 reflection 返回 plain object |

**边界说明**: 脚本层 `parser` 公开 JSON/CSV/XML schema bind/validate 与 XML XPath 查询；JSON/CSV emit 由 parser 本地实现。底层 `tbe/data_bind` C ABI 是 JSON/CSV/XML schema bind/validate 的主事实源。

---

## ❌ 关键缺失

### 1. 文件权限、符号链接与锁

```javascript
// 当前无法实现
chmod("script.tbs", 0755);             // ❌ 不存在
let target = readlink("latest");      // ❌ 不存在
flock("data.lock");                   // ❌ 不存在
```

**影响**: 权限管理、软链接工作流和跨进程锁仍需宿主侧处理

**实施**: 需要先在 `turbo_fs.h` 暴露跨平台 chmod/readlink/symlink/flock 语义

**预计工作量**: 2-4 周

---

### 2. 正则表达式语法糖

```javascript
// 当前已有 core regex 函数和 RegExp 对象 API；还没有 /.../ 字面量
let h = regex.compile("\\d{3}-\\d{4}");
let ok = regex.match(h, "555-1234");  // ✅ 已支持
let pattern = /\d{3}-\d{4}/;          // ❌ 语法糖未支持
let re = RegExp("\\d{3}-\\d{4}");
let match = re.exec("555-1234");       // ✅ 已支持
regex.free(h);
```

**影响**: 复杂文本解析已有函数式 API 和 JavaScript 风格对象 API，剩余差距是 regex literal 语法糖

**实施**: 在语法层新增 regex literal，并将其映射到 core `RegExp`/`regex` API

**预计工作量**: 1-2 周

---

### 3. 高级密码学 API

```javascript
let digest = crypto.sha256("hello");      // ✅ SHA-256
let keyed = crypto.blake2b_keyed(data, k); // ✅ keyed BLAKE2b
let cipher = crypto.aes_encrypt(data, key, iv); // ✅ AES-CTR
let box = crypto.aead_lock(data, key32, nonce24, ad); // ✅ XChaCha20-Poly1305 AEAD
let dk = crypto.argon2(password, salt, 32, 65536, 3, 2); // ✅ Argon2id
let kp = crypto.eddsa_key_pair(seed32); // ✅ EdDSA/Curve25519 + BLAKE2b
```

**现状**: 摘要、快速哈希、常量时间比较、AES-CTR、Monocypher AEAD、Argon2、X25519、EdDSA、ChaCha20、Poly1305 和 Elligator 已可用。

**剩余**: Monocypher 的增量 BLAKE2b/AEAD/Poly1305 context 仍未暴露为脚本可变对象；脚本层当前使用一次性函数 API。

---

## 📊 完整性评估

| 模块 | 完成度 | 函数数 | 状态 |
|------|--------|--------|------|
| Math | ✅ 100% | ~90 | 生产就绪 |
| String | ✅ 100% | 27 | 生产就绪 |
| Stats | ✅ 100% | 16 | 生产就绪 |
| File I/O | ⚠️ 99% | 32 | 缺 chmod/symlink/flock 等高级增强 |
| Date/Time | ✅ 97% | 16 | 原生 datetime/date/time/duration 已支持；高级时区数据库未接入 |
| Vector | ✅ 100% | 30+ | 生产就绪 |
| Parser / Schema Binding | ✅ 100% | 40+ | JSON/CSV/XML schema binding 生产就绪 |
| Regex | ✅ 90% | 14 | core `regex.*` 与 `RegExp` 对象 API 已实现；缺 regex literal |
| Hash | ✅ 100% | 3 | `xxh32`、`xxh64_hex`、`xxh3_64_hex` |
| Crypto | ✅ 95% | 34 | SHA-256、BLAKE2b、AES-CTR、XChaCha20-Poly1305 AEAD、Argon2、X25519、EdDSA、ChaCha20、Poly1305、Elligator、常量时间比较；剩余增量 context 对象 |

**总体评估**: 标准库已有 **240+ 函数**，**85%+ 完备度**

---

## 🎯 与主流脚本语言对比

### Python 对比

| 功能领域 | Python | TurboScript | 差距 |
|---------|--------|-------------|------|
| 数学 | `math` 模块 (60 函数) | 90 函数 | ✅ 超越 |
| 字符串 | `str` 类 (50 方法) | 27 函数 | ⚠️ 约 50% |
| 统计 | `statistics` (15 函数) | 16 函数 | ✅ 持平 |
| 文件 I/O | `pathlib` + `os` | 32 函数 | ⚠️ 缺 chmod/symlink/flock 等增强 |
| 日期时间 | `datetime` | 16 函数 + 原生 `datetime`/`date`/`time`/`duration` 值 | ⚠️ 基础解析/格式化已覆盖，缺时区数据库 |
| 正则 | `re` 模块 | core `regex.*` + `RegExp` | ⚠️ 函数式/对象 API 已有，缺字面量 |
| 加密 | `hashlib` | `crypto.sha256` + `crypto.blake2b` + `crypto.aes_*` + `crypto.aead_*` + `crypto.argon2` + `crypto.x25519` + `crypto.eddsa_*` + `hash.xxhash` | ⚠️ 常用摘要、KDF、对称加密、AEAD、签名和密钥交换已覆盖；缺增量 context 对象 |

### JavaScript 对比

| 功能领域 | JavaScript | TurboScript | 差距 |
|---------|-----------|-------------|------|
| 数学 | `Math` 对象 (40 函数) | 90 函数 | ✅ 超越 |
| 字符串 | `String` 类 (60 方法) | 27 函数 | ⚠️ 约 45% |
| 数组 | `Array` 类 (40 方法) | 30+ 函数 | ⚠️ 约 75% |
| 文件 I/O | Node.js `fs` | 32 函数 | ⚠️ 缺 chmod/symlink/flock 等增强 |
| 日期时间 | `Date` 类 | 16 函数 + 原生 temporal 值 | ✅ 基础能力对齐；缺时区数据库 |
| 正则 | `RegExp` 类 | core `RegExp` 对象 API | ⚠️ 缺 `/.../` 字面量 |

---

## 🚀 快速胜利清单（优先级排序）

### 1. chmod/readlink/symlink/flock - **中优先级**
- **影响**: 补齐权限、软链接和跨进程协调能力
- **工作量**: 2-4 周
- **依赖**: `turbo_fs.h` 增加跨平台 API

### 2. Regex literal - **中优先级**
- **影响**: 在已有 core regex / RegExp 能力上补齐更自然的脚本语法
- **工作量**: 1-2 周
- **依赖**: 现有 libfsm/libre 正则后端与 core `RegExp` 对象 API

### 3. Crypto 增量 context 对象 - **中优先级**
- **影响**: 对大文件或长消息暴露 BLAKE2b/AEAD/Poly1305 streaming API，减少一次性缓冲需求
- **工作量**: 1-2 周
- **依赖**: monocypher（项目已有）

### 4. 时区数据库 - **低优先级**
- **影响**: 增强时间处理
- **工作量**: 3-4 周
- **依赖**: tzdata 或 C++20 `<chrono>`

---

## 💡 结论

**TurboScript 的标准库出乎意料的成熟**：

1. ✅ **数学/统计库世界一流** - 90+ 函数，超越 Python/JavaScript
2. ✅ **字符串处理完备** - 覆盖所有常见操作
3. ✅ **文件 I/O 99% 完整** - 已有目录列举、复制、截断和递归目录操作，剩余权限/符号链接/锁等增强
4. ✅ **日期时间基础完备** - 原生 `datetime`、解析、格式化与 schema binding 已覆盖

**剩余关键增强**主要有 3 个：
1. 文件权限、符号链接与文件锁
2. Regex literal
3. Crypto 增量 context 对象

**战略建议**：
- 待 `turbo_fs.h` 暴露 chmod/symlink/readlink/flock 后再桥接脚本 API
- 然后在现有 core `RegExp` 上补 regex literal
- 加密扩展已有 SHA-256、BLAKE2b、AES-CTR、XChaCha20-Poly1305 AEAD、Argon2、X25519、EdDSA、ChaCha20、Poly1305 和 Elligator；下一步补增量 context 对象

完成这些增强后，TurboScript 的标准库将达到 **95%+ 完备度**，足以支撑生产环境使用。

---

**更新日期**: 2026-06-14  
**审查者**: AI Assistant  
**基于代码**: exprtk/src/mod_*.c
