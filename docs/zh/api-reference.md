# TurboScript API 参考

所有内置函数和标准库的完整参考。

---

## 目录

1. [数学函数](#数学函数)
2. [统计函数](#统计函数)
3. [字符串函数](#字符串函数)
4. [向量函数](#向量函数)
5. [线性代数](#线性代数)
6. [文件 I/O](#文件-io)
7. [日期和时间](#日期和时间)
8. [类型转换](#类型转换)
9. [平台函数](#平台函数)

模块特定函数（CSV、JSON、TA 等），请参见[模块文档](../modules/)。

---

## 数学函数

### 基础数学

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `abs(x)` | 绝对值 | `abs(-5)` → `5` |
| `sqrt(x)` | 平方根 | `sqrt(16)` → `4` |
| `cbrt(x)` | 立方根 | `cbrt(27)` → `3` |
| `pow(x, y)` | 幂运算 (x^y) | `pow(2, 3)` → `8` |
| `exp(x)` | e^x | `exp(1)` → `2.718...` |
| `exp2(x)` | 2^x | `exp2(3)` → `8` |
| `log(x)` | 自然对数 | `log(e)` → `1` |
| `log2(x)` | 以 2 为底的对数 | `log2(8)` → `3` |
| `log10(x)` | 以 10 为底的对数 | `log10(100)` → `2` |

### 三角函数

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `sin(x)` | 正弦（弧度） | `sin(pi/2)` → `1` |
| `cos(x)` | 余弦（弧度） | `cos(0)` → `1` |
| `tan(x)` | 正切（弧度） | `tan(pi/4)` → `1` |
| `asin(x)` | 反正弦 | `asin(1)` → `pi/2` |
| `acos(x)` | 反余弦 | `acos(1)` → `0` |
| `atan(x)` | 反正切 | `atan(1)` → `pi/4` |
| `atan2(y, x)` | y/x 的反正切 | `atan2(1, 1)` → `pi/4` |

### 取整

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `ceil(x)` | 向上取整 | `ceil(3.2)` → `4` |
| `floor(x)` | 向下取整 | `floor(3.8)` → `3` |
| `round(x)` | 四舍五入 | `round(3.5)` → `4` |
| `trunc(x)` | 截断为整数 | `trunc(3.9)` → `3` |

### 实用函数

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `mod(a, b)` | 取模 | `mod(7, 3)` → `1` |
| `clamp(x, lo, hi)` | 限制 x 在 [lo, hi] 范围内 | `clamp(15, 0, 10)` → `10` |
| `lerp(a, b, t)` | 线性插值 | `lerp(0, 10, 0.5)` → `5` |
| `rand()` | 随机数 [0, 1) | `rand()` → `0.742...` |

---

## 统计函数

### 聚合

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `sum(v)` | 元素之和 | `sum([1,2,3])` → `6` |
| `avg(v)` | 平均值 | `avg([1,2,3])` → `2` |
| `min(v)` | 最小值 | `min([3,1,2])` → `1` |
| `max(v)` | 最大值 | `max([3,1,2])` → `3` |
| `len(v)` | 元素数量 | `len([1,2,3])` → `3` |

### 分布

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `median(v)` | 中位数 | `median([1,2,3,4,5])` → `3` |
| `percentile(v, p)` | 第 p 百分位数 (0-100) | `percentile([1,2,3,4,5], 75)` → `4` |

### 转换

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `cumsum(v)` | 累积和 | `cumsum([1,2,3])` → `[1,3,6]` |
| `sort(v)` | 升序排序 | `sort([3,1,2])` → `[1,2,3]` |

---

## 字符串函数

### 大小写转换

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `lower(s)` | 转换为小写 | `lower("HELLO")` → `"hello"` |
| `upper(s)` | 转换为大写 | `upper("hello")` → `"HELLO"` |

### 修剪

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `trim(s)` | 修剪空白 | `trim("  hi  ")` → `"hi"` |
| `ltrim(s)` | 修剪左侧空白 | `ltrim("  hi")` → `"hi"` |
| `rtrim(s)` | 修剪右侧空白 | `rtrim("hi  ")` → `"hi"` |

### 搜索

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `contains(s, sub)` | 检查是否包含子串 | `contains("hello", "ell")` → `1` |
| `starts_with(s, prefix)` | 检查是否以...开头 | `starts_with("hello", "he")` → `1` |
| `ends_with(s, suffix)` | 检查是否以...结尾 | `ends_with("hello", "lo")` → `1` |
| `index_of(s, sub)` | 查找第一次出现的位置 | `index_of("hello", "l")` → `2` |
| `last_index_of(s, sub)` | 查找最后一次出现的位置 | `last_index_of("hello", "l")` → `3` |
| `find(s, sub [, start])` | Python 风格正向查找 | `find("ababa", "ba", 2)` → `3` |
| `rfind(s, sub [, end])` | 反向查找，可限制前缀范围 | `rfind("ababa", "ba")` → `3` |
| `find_all(s, sub)` | 返回非重叠子串的字节下标列表 | `find_all("aaaa", "aa")` → `[0,2]` |
| `find_all_overlapping(s, sub)` | 返回重叠子串的字节下标列表 | `find_all_overlapping("aaaa", "aa")` → `[0,1,2]` |

### 操作

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `substr(s, start, len)` | 提取子串 | `substr("hello", 1, 3)` → `"ell"` |
| `slice(s, start [, end])` | 支持负索引的字节切片 | `slice("abcdef", -4, -1)` → `"cde"` |
| `char_at(s, index)` | 返回索引处的单字节字符串 | `char_at("abc", 1)` → `"b"` |
| `byte_at(s, index)` | 返回索引处的字节值 | `byte_at("Az", 1)` → `122` |
| `byte_length(s)` | 字节长度别名 | `byte_length("你")` → `3` |
| `utf8_slice(s, start [, end])` | 支持负索引的 UTF-8 码点切片 | `utf8_slice("a你b", -2)` → `"你b"` |
| `ord(s [, index])` | 返回 UTF-8 码点值 | `ord("你")` → `20320` |
| `chr(codepoint)` | 单个码点转 UTF-8 字符串 | `chr(20320)` → `"你"` |
| `from_codepoint(cp...)` | 多个码点转 UTF-8 字符串 | `from_codepoint(20320,128578)` → `"你🙂"` |
| `codepoint_at(s, index)` | 带索引的 `ord` 别名 | `codepoint_at("a你", 1)` → `20320` |
| `replace(s, from, to)` | 替换所有出现 | `replace("aabbcc", "bb", "XX")` → `"aaXXcc"` |
| `replace_all(s, from, to)` | `replace` 的别名 | `replace_all("aabb", "a", "x")` → `"xxbb"` |
| `replace_range(s, start, len, value)` | 替换字节范围 | `replace_range("abcdef", 2, 3, "X")` → `"abXf"` |
| `insert(s, index, value)` | 在字节位置插入 | `insert("ab", 1, "X")` → `"aXb"` |
| `delete_range(s, start, len)` | 删除字节范围 | `delete_range("abcdef", 2, 3)` → `"abf"` |
| `reverse(s)` | 反转字符串 | `reverse("abc")` → `"cba"` |
| `repeat(s, count)` | `str_repeat` 的别名 | `repeat("ab", 2)` → `"abab"` |
| `count(s, sub)` | 子串计数别名 | `count("aaaa", "aa")` → `2` |
| `count_overlapping(s, sub)` | 统计重叠子串出现次数 | `count_overlapping("aaaa", "aa")` → `3` |
| `left(s, count)` / `str_take(s, count)` | 取左侧内容；UTF-8 合法时按码点 | `left("a你b", 2)` → `"a你"` |
| `right(s, count)` | 取右侧内容；UTF-8 合法时按码点 | `right("a你b", 2)` → `"你b"` |
| `str_drop(s, count)` | 丢弃左侧内容；UTF-8 合法时按码点 | `str_drop("a你b", 1)` → `"你b"` |
| `normalize_space(s)` | 去掉首尾空白并折叠 ASCII 空白 | `normalize_space(" a\t b ")` → `"a b"` |
| `center(s, width [, fill])` | 居中；UTF-8 合法时按码点宽度计算 | `center("x", 3, ".")` → `".x."` |
| `truncate(s, max_len [, suffix])` | 截断；UTF-8 合法时按码点长度计算 | `truncate("abcdef", 5)` → `"ab..."` |
| `zfill(s, width)` | 在可选符号后用 0 左填充 | `zfill("-42", 5)` → `"-0042"` |
| `expand_tabs(s [, tabsize])` | 将 tab 扩展为空格 | `expand_tabs("a\tb", 4)` → `"a   b"` |
| `chomp(s)` | 移除一个尾部换行 | `chomp("a\r\n")` → `"a"` |
| `split_lines(s)` | `str_lines` 的别名 | `split_lines("a\nb")` → `["a","b"]` |
| `line_count(s)` | 按 `str_lines` 语义统计逻辑行 | `line_count("a\nb")` → `2` |
| `indent(s, prefix [, first])` | 为每行添加前缀；`first=0` 跳过首行 | `indent("a\nb", "> ")` → `"> a\n> b"` |
| `dedent(s)` / `unindent(s)` | 移除非空行共同的前导空格/tab | `dedent("  a\n    b")` → `"a\n  b"` |
| `surround(s, prefix [, suffix])` | 添加成对前后缀 | `surround("x", "[", "]")` → `"[x]"` |
| `unwrap(s, prefix [, suffix])` | 当前后缀都匹配时移除 | `unwrap("[x]", "[", "]")` → `"x"` |
| `word_wrap(s, width [, break_long])` | 按字节宽度做 ASCII 单词换行 | `word_wrap("alpha beta", 5)` → `"alpha\nbeta"` |
| `shorten(s, width [, suffix])` | 按单词边界缩短字符串 | `shorten("alpha beta", 8)` → `"alpha..."` |
| `snake_case(s)` / `kebab_case(s)` | 转为分隔符字段名 | `snake_case("Hello World")` → `"hello_world"` |
| `camel_case(s)` / `pascal_case(s)` | 转为 camel/Pascal 命名 | `camel_case("hello world")` → `"helloWorld"` |
| `slugify(s)` | 转为 URL slug | `slugify("Hello, Turbo!")` → `"hello-turbo"` |
| `strip(s)` / `lstrip(s)` / `rstrip(s)` | trim 系列别名 | `strip(" hi ")` → `"hi"` |
| `strip_prefix(s, prefix)` | `remove_prefix` 的别名 | `strip_prefix("pre-x", "pre-")` → `"x"` |
| `strip_suffix(s, suffix)` | `remove_suffix` 的别名 | `strip_suffix("x.txt", ".txt")` → `"x"` |
| `padStart(s, width [, fill])` / `padEnd(...)` | JS 风格 padding 别名 | `padStart("x", 3, "0")` → `"00x"` |
| `split_whitespace(s)` | 按 ASCII 空白分割为字符串列表 | `split_whitespace(" a b ")` → `["a","b"]` |
| `split_once(s, delim)` | 按首个分隔符切为 `[before, after]` | `split_once("a=b=c", "=")` → `["a","b=c"]` |
| `rsplit_once(s, delim)` | 按最后一个分隔符切为 `[before, after]` | `rsplit_once("a=b=c", "=")` → `["a=b","c"]` |
| `split_limit(s, delim, maxsplit)` | 从左向右按最大次数分割 | `split_limit("a,b,c", ",", 1)` → `["a","b,c"]` |
| `rsplit(s, delim [, maxsplit])` | 从右向左分割为字符串列表 | `rsplit("a,b,c", ",", 1)` → `["a,b","c"]` |

### 谓词

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `is_empty(s)` | 字符串长度为 0 | `is_empty("")` → `1` |
| `is_blank(s)` | 为空或只包含 ASCII 空白 | `is_blank(" \t")` → `1` |
| `is_ascii(s)` | 所有字节都是 ASCII | `is_ascii("abc")` → `1` |
| `is_digit(s)` | 非空且只包含 ASCII 数字 | `is_digit("123")` → `1` |
| `is_hex(s)` | 非空且只包含 ASCII 十六进制字符 | `is_hex("0aF9")` → `1` |
| `is_alpha(s)` | 非空且只包含 ASCII 字母 | `is_alpha("abc")` → `1` |
| `is_alnum(s)` | 非空且只包含 ASCII 字母或数字 | `is_alnum("abc123")` → `1` |
| `is_space(s)` | 非空且只包含 ASCII 空白 | `is_space("\n")` → `1` |
| `is_printable(s)` | ASCII 可打印字符加常见空白 | `is_printable("a\n")` → `1` |
| `is_lower(s)` | 包含小写字母且没有大写字母 | `is_lower("abc1")` → `1` |
| `is_upper(s)` | 包含大写字母且没有小写字母 | `is_upper("ABC1")` → `1` |
| `constant_time_eq(a, b)` | 不提前退出的字节相等比较 | `constant_time_eq("x","x")` → `1` |

### 编码与转义

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `html_escape(s)` | 转义 HTML 敏感字符 | `html_escape("<a&>")` → `"&lt;a&amp;&gt;"` |
| `html_unescape(s)` | 解码常见 HTML entity | `html_unescape("&lt;")` → `"<"` |
| `json_escape(s)` | 转义 JSON 字符串字面量内容 | `json_escape("a\n")` → `"a\\n"` |
| `json_unescape(s)` | 解码 JSON 字符串字面量 escape | `json_unescape("\\u4F60")` → `"你"` |
| `url_encode(s)` | 对 UTF-8 字节做 percent encode | `url_encode("a b")` → `"a%20b"` |
| `url_decode(s)` | 解码 percent escape（`+` 变为空格） | `url_decode("a%20b")` → `"a b"` |
| `base64_encode(s)` | Base64 编码字节 | `base64_encode("hello")` → `"aGVsbG8="` |
| `base64_decode(s)` | Base64 解码为字符串 | `base64_decode("aGVsbG8=")` → `"hello"` |
| `base64url_encode(s)` | 无填充的 URL 安全 Base64 编码 | `base64url_encode("hello?")` → `"aGVsbG8_"` |
| `base64url_decode(s)` | 解码 URL 安全 Base64 | `base64url_decode("aGVsbG8_")` → `"hello?"` |
| `hex_encode(s)` | 小写十六进制编码字节 | `hex_encode("Az")` → `"417a"` |
| `hex_decode(s)` | 十六进制解码为字符串 | `hex_decode("417a")` → `"Az"` |
| `bytes(s)` | 返回字节值列表 | `bytes("Az")` → `[65,122]` |
| `from_bytes(list_or_vector)` | 从字节值构造字符串 | `from_bytes([65,122])` → `"Az"` |

### Hash 插件

使用 `import("hash")` 加载。

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `hash.xxh32(s)` | xxHash32，返回可精确表示的数字 | `hash.xxh32("abc")` |
| `hash.xxh32(s, seed)` | 带 seed 的 xxHash32 | `hash.xxh32("abc", 7)` |
| `hash.xxh64_hex(s)` | xxHash64，返回 16 位小写十六进制字符串 | `hash.xxh64_hex("abc")` |
| `hash.xxh64_hex(s, seed)` | 带 seed 的 xxHash64 十六进制结果 | `hash.xxh64_hex("abc", 7)` |
| `hash.xxh3_64_hex(s)` | XXH3 64 位哈希，返回 16 位小写十六进制字符串 | `hash.xxh3_64_hex("abc")` |
| `hash.xxh3_64_hex(s, seed)` | 带 seed 的 XXH3 64 位十六进制结果 | `hash.xxh3_64_hex("abc", 7)` |

### Crypto 插件

使用 `import("crypto")` 加载。哈希输出均为小写十六进制字符串。

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `crypto.blake2b(s)` | BLAKE2b，64 字节 digest | `crypto.blake2b("abc")` |
| `crypto.blake2b(s, size)` | BLAKE2b，digest 大小为 `1..64` 字节 | `crypto.blake2b("abc", 4)` → `"63906248"` |
| `crypto.blake2b_keyed(s, key)` | 带 key 的 BLAKE2b，64 字节 digest | `crypto.blake2b_keyed("abc", "key")` |
| `crypto.blake2b_keyed(s, key, size)` | 带 key 的 BLAKE2b，digest 大小为 `1..64` 字节 | `crypto.blake2b_keyed("abc", "key", 4)` → `"34d401b4"` |
| `crypto.verify16(a, b)` | 常量时间比较两个 16 字节字符串 | `crypto.verify16(mac1, mac2)` |
| `crypto.verify32(a, b)` | 常量时间比较两个 32 字节字符串 | `crypto.verify32(d1, d2)` |
| `crypto.verify64(a, b)` | 常量时间比较两个 64 字节字符串 | `crypto.verify64(d1, d2)` |

### 解析

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `split(s, delim)` | 分割为向量 | `split("1,2,3", ",")` → `[1,2,3]` |

### 模板渲染

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `template_render(template, data)` | 使用原生脚本数据渲染 Mustache 模板 | `template_render("Hi {{name}}", map{name:"Ada"})` → `"Hi Ada"` |
| `template_render(template, data, partials)` | 使用 partial 模板 map 渲染 | `template_render("{{>item}}", data, map{item:"{{name}}"})` |
| `mustache_render(template, data)` | `template_render` 的别名 | `mustache_render("{{name}}", data)` |

`data` 可以是 map、list、vector、标量或函数值。map 支持字段查找和
`{{user.city}}` 这样的点路径。list 与 vector 支持
`{{#items}}{{name}}{{/items}}` 形式的 section；真值标量会让 section 渲染一次。
`{{name}}` 会做 HTML 转义，`{{{name}}}` 输出原始文本。缺失字段和缺失 partial
输出为空。partial 名也支持进入 `partials` map 的点路径，例如 `{{>layout.item}}`。

函数值按 Mustache lambda 处理。插值 lambda 接收空字符串；section lambda 接收原始
section 内容。lambda 返回值会转为文本，并在当前模板上下文中继续渲染。

```javascript
var data = map{
    name: "Ada",
    user: map{city: "Paris"},
    items: list(map{name: "one"}, map{name: "two"}),
    wrap: func(raw) { return "[" + raw + ":" + "{{name}}" + "]"; }
};

var partials = map{item: "{{name}};"};
var out = template_render(
    "Hi {{name}} {{user.city}} {{#items}}{{>item}}{{/items}} {{#wrap}}body{{/wrap}}",
    data,
    partials
);
// "Hi Ada Paris one;two; [body:Ada]"
```

### 点号风格方法

所有字符串函数都支持点号风格语法：

```javascript
"HELLO".lower()              // "hello"
"hello".upper()              // "HELLO"
"  hi  ".trim()              // "hi"
"hello".contains("ell")      // 1
"hello".indexOf("l")         // 2
"hello".substr(1, 3)         // "ell"
"aabbcc".replace("bb", "XX") // "aaXXcc"
"hello".length()             // 5
"abc".reverse()              // "cba"
```

---

## 向量函数

### 基本操作

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `size(v)` | 元素数量 | `size([1,2,3])` → `3` |
| `len(v)` | size 的别名 | `len([1,2,3])` → `3` |

### 搜索

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `vector_find_value(v, val)` | 值的第一个索引 | `vector_find_value([10,20,30], 20)` → `1` |
| `vector_find_all(v, val)` | 值的所有索引 | `vector_find_all([10,20,10], 10)` → `[0,2]` |

### 点号风格方法

```javascript
var v = [10, 20, 30, 40, 50];

// 内置方法
v.length()              // 5
v.push(60)              // 追加元素（修改）
v.pop()                 // 移除最后一个（修改）
v.indexOf(30)           // 2
v.reverse()             // 反转副本

// 注册表分发方法
v.sum()                 // 150
v.avg()                 // 30
v.min()                 // 10
v.max()                 // 50
v.sort()                // 排序副本
v.median()              // 30
v.cumsum()              // [10, 30, 60, 100, 150]
```

---

## 线性代数

矩阵默认以**列主序**存储在扁平向量中；大多数 `matrix.*` / `linalg.*`
函数可在最后一个参数传 `1` 使用行主序。

### 2×2 矩阵

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `det2(m)` | 行列式 | `det2([1,2,3,4])` → `-2` |
| `inv2(m)` | 逆矩阵 | `inv2([1,2,3,4])` → `[-2,1,1.5,-0.5]` |

### 3×3 矩阵

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `det3(m)` | 行列式 | `det3([1,0,0,0,1,0,0,0,1])` → `1` |
| `inv3(m)` | 逆矩阵 | `inv3([...])` → `[...]` |

### 通用操作

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `matmul(A, B, m, k, n[, layout])` | 矩阵乘法 (m×k) × (k×n) | `matmul(A, B, 2, 2, 2)` |
| `matrix.det(A, n[, layout])` | 通用 n×n 行列式 | `matrix.det(A, 3)` |
| `matrix.inv(A, n[, layout])` | 通用 n×n 逆矩阵 | `matrix.inv(A, 3)` |
| `matrix.solve(A, B, n, cols[, layout])` | 求解 `A * X = B` | `matrix.solve(A, b, 3, 1)` |
| `matrix.rank(A, rows, cols[, tol][, layout])` | 数值秩 | `matrix.rank(A, 3, 2)` |
| `matrix.cond(A, rows, cols[, layout])` | 基于奇异值的 2-范数条件数估计 | `matrix.cond(A, 3, 2)` |
| `matrix.slogdet(A, n[, layout])` | 返回 `[sign, log(abs(det))]` | `matrix.slogdet(A, 3)` |
| `transpose(m, rows, cols)` | 转置矩阵 | `transpose([1,2,3,4], 2, 2)` → `[1,3,2,4]` |

### 线代分解

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `linalg.qr(A, rows, cols[, layout])` | QR 分解，返回 `[Q, R]` | `linalg.qr(A, 3, 2)` |
| `linalg.lu(A, n[, layout])` | 带部分主元的 LU 分解，返回 `[L, U, pivots]` | `linalg.lu(A, 3)` |
| `linalg.lu_solve(A, B, n, cols[, layout])` | 通过 LU 求解线性方程组 | `linalg.lu_solve(A, b, 3, 1)` |
| `linalg.solve_cholesky(A, B, n, cols[, layout])` | 通过 Cholesky 求解 SPD 方程组 | `linalg.solve_cholesky(A, b, 3, 1)` |
| `linalg.det_lu(A, n[, layout])` | 通过 LU 计算行列式 | `linalg.det_lu(A, 3)` |
| `linalg.eigh(A, n[, layout])` | 对称矩阵特征值，升序返回 | `linalg.eigh(A, 3)` |
| `linalg.svd(A, rows, cols[, layout])` | 奇异值，降序返回 | `linalg.svd(A, 3, 2)` |
| `linalg.pinv(A, rows, cols[, layout])` | 伪逆 | `linalg.pinv(A, 4, 2)` |
| `linalg.lstsq(A, b, rows, cols[, layout])` | 最小二乘解 | `linalg.lstsq(A, b, 4, 2)` |

**示例：**
```javascript
// 2×2 列主序矩阵：[[1, 3], [2, 4]]
var A = [1, 2, 3, 4];

// 行列式
var d = det2(A);  // -2

// 逆矩阵
var inv = inv2(A);  // [-2, 1, 1.5, -0.5]

// 矩阵乘法：A × A
var result = matmul(A, A, 2, 2, 2);  // [7, 10, 15, 22]

// 显式行主序输入/输出
var row = matmul([1, 2, 3, 4], [5, 6, 7, 8], 2, 2, 2, 1); // [19, 22, 43, 50]

var lu = linalg.lu([4, 2, 2, 3], 2);
var eig = linalg.eigh([2, 1, 1, 2], 2); // [1, 3]
var sv = linalg.svd([1, 0, 0, 0, 2, 0], 3, 2); // [2, 1]
```

---

## 文件 I/O

### 文件操作

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `read_file(path)` | 读取文件内容 | 字符串（错误时为 0） |
| `write_file(path, content)` | 写入文件 | 成功时为 0 |
| `append_file(path, content)` | 追加到文件 | 成功时为 0 |
| `copy_file(src, dst)` | 复制文件内容 | 成功时为 0 |
| `file_exists(path)` | 检查文件是否存在 | 存在为 1，否则为 0 |
| `file_size(path)` | 获取文件大小 | 字节数（错误时为 -1） |
| `file_truncate(path, length)` | 截断或扩展文件 | 成功时为 0 |
| `is_file(path)` | 检查是否为常规文件 | 是文件为 1，否则为 0 |
| `is_dir(path)` | 检查是否为目录 | 是目录为 1，否则为 0 |
| `file_remove(path)` | 删除文件 | 成功时为 0 |
| `file_rename(old, new)` | 重命名/移动文件 | 成功时为 0 |

**示例：**
```javascript
// 写入文件
write_file("output.txt", "Hello, World!");

// 读取文件
var content = read_file("output.txt");
print(content);  // "Hello, World!"

// 检查存在
if (file_exists("data.txt")) {
    var size = file_size("data.txt");
    print("文件大小: " + size + " 字节");
}

// 删除文件
file_remove("temp.txt");
```

### 目录操作

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `listdir(path)` | 列出目录项名称 | List |
| `glob(pattern)` | 列出匹配通配符模式的路径 | List |
| `mkdir(path [, mode])` | 创建目录 | 成功时为 0 |
| `mkdir_recursive(path [, mode])` | 创建目录及缺失的父目录 | 成功时为 0 |
| `rmdir(path)` | 删除目录 | 成功时为 0 |
| `rmdir_recursive(path)` | 删除目录树 | 成功时为 0 |
| `tmpdir()` | 获取临时目录路径 | 字符串 |

### 路径工具

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `path_join(base, rel)` | 连接路径组件 | 字符串 |
| `path_dirname(path)` | 获取目录部分 | 字符串 |
| `path_basename(path)` | 获取文件名部分 | 字符串 |
| `path_is_absolute(path)` | 检查是否为绝对路径 | 是绝对路径为 1，否则为 0 |

**示例：**
```javascript
var full = path_join("/home/user", "data.csv");  // "/home/user/data.csv"
var dir = path_dirname(full);                     // "/home/user"
var file = path_basename(full);                   // "data.csv"
```

---

## 日期和时间

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `now()` | 当前 Unix 时间戳 | 自纪元以来的秒数 |
| `date(str)` | 使用 TurboNet datetime parser 解析日期字符串 | Unix 时间戳 |
| `date_utc(str)` | 按 UTC 解析 `YYYY-MM-DD`、`YYYY-MM-DD HH:MM:SS` 或 `YYYY-MM-DDTHH:MM:SSZ` | Unix 时间戳 |
| `format_date(ts [, fmt])` | 格式化时间戳 | 字符串（默认 RFC 822；自定义格式使用宿主本地时间） |
| `format_date_utc(ts [, fmt])` | 按 UTC 格式化时间戳 | 字符串（默认 RFC 822；自定义格式使用 UTC） |

时区约定：

- `now()` 返回 Unix 时间戳，本身不带时区。
- `date(str)` 使用 TurboNet 的通用 datetime parser。
- `format_date(ts, fmt)` 为兼容旧脚本保留宿主本地时间格式化。
- 需要确定性 UTC 行为时，使用 `date_utc(str)` 与 `format_date_utc(ts, fmt)`。
- 需要结构化日期字段时，加载 `parser` 模块并使用 `datetime.parse()`。

**示例：**
```javascript
var current = now();                              // 1709568000
var parsed = date_utc("2024-01-01 12:00:00");    // 1704110400
var formatted = format_date_utc(parsed, "%Y-%m-%d %H:%M:%S"); // "2024-01-01 12:00:00"
```

---

## 类型转换

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `to_num(s)` | 字符串转数字 | `to_num("123.45")` → `123.45` |
| `to_str(x)` | 数字转字符串 | `to_str(42)` → `"42"` |
| `to_int(s)` | 字符串转整数 | `to_int("99")` → `99` |
| `to_bool(s)` | 字符串转布尔值 | `to_bool("true")` → `1` |

---

## 平台函数

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `os_name()` | 操作系统 | `"windows"`、`"linux"`、`"macos"` 或 `"unknown"` |
| `pid()` | 进程 ID | 整数 |
| `uptime_ms()` | 进程启动以来的毫秒数 | 整数 |
| `monotonic_ms()` | 单调时钟毫秒数 | 整数 |

**示例：**
```javascript
var os = os_name();
if (os == "windows") {
    print("运行在 Windows 上");
} elif (os == "linux") {
    print("运行在 Linux 上");
}

var process_id = pid();
var uptime = uptime_ms();
print("进程 " + process_id + " 已运行 " + uptime + "ms");
```

---

## 模块特定函数

对于可选模块提供的函数：

- **Parser / CSV / JSON / Datetime**：参见 [parser README](../../modules/parser/README.md)
- **CSV 过滤表达式**：参见 [csv_filter_expression.md](../csv_filter_expression.md)
- **技术分析**：参见 [ta_fin_cheatsheet.md](../ta_fin_cheatsheet.md)
- **向量操作**：参见 [vec_cheatsheet.md](../vec_cheatsheet.md)
- **金融 / 策略**：参见 [ta_fin_cheatsheet.md](../ta_fin_cheatsheet.md) 和 [modules/FIN_MODULE.md](../modules/FIN_MODULE.md)

---

## 另请参阅

- **[语言指南](language-guide.md)** - 完整语法参考
- **[快速开始](getting-started.md)** - 快速教程
- **[插件开发](plugin-development.md)** - 使用 C/C++ 扩展

---

**TurboScript 完整 API 文档**
