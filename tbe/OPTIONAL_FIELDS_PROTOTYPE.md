# 可选字段实现原型

## 语法扩展

### 1. Lexer扩展 (schema_lexer.re)

```c
// 添加新关键字
"required" {
    token->type = SCHEMA_TOKEN_REQUIRED;
    token->value = token_start;
    token->length = (size_t)(YYCURSOR - token_start);
    lexer->cursor = YYCURSOR;
    return 1;
}

"optional" {
    token->type = SCHEMA_TOKEN_OPTIONAL;
    token->value = token_start;
    token->length = (size_t)(YYCURSOR - token_start);
    lexer->cursor = YYCURSOR;
    return 1;
}
```

### 2. Parser扩展 (schema_grammar.y)

```c
%token REQUIRED OPTIONAL.

// 修改字段声明规则
field_decl ::= field_qualifier(Q) IDENT(T) IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == SCHEMA_TOKEN_OPTIONAL);
    add_field_with_optional(ctx, type_name, field_name, is_optional, NULL, 0);
    free(type_name);
    free(field_name);
}

field_qualifier(Q) ::= REQUIRED. { Q = SCHEMA_TOKEN_REQUIRED; }
field_qualifier(Q) ::= OPTIONAL. { Q = SCHEMA_TOKEN_OPTIONAL; }
field_qualifier(Q) ::= .         { Q = SCHEMA_TOKEN_REQUIRED; } // 默认required
```

### 3. 数据结构扩展

```c
// 在 schema_types.h 中添加
typedef struct {
    // ... 现有字段 ...
    int is_optional;        // 是否为可选字段
    int optional_bit_index; // 在位图中的位置
} field_info_t;

// 在解析上下文中跟踪
typedef struct {
    // ... 现有字段 ...
    int optional_field_count;  // 当前message的可选字段数
} schema_parse_ctx_t;
```

## 代码生成示例

### 输入Schema

```c
schema UserService [id(1), version(1), byte_order(little)];

message User {
    required uint32 id;
    required string username;
    optional string email;
    optional uint32 age;
    optional string phone;
}
```

### 生成的C代码

```c
// ============================================================================
// User Message
// ============================================================================

#define User_OPTIONAL_FIELD_COUNT 3
#define User_PRESENCE_BITMAP_SIZE 1  // (3 + 7) / 8

typedef enum {
    User_OPTIONAL_email = 0,
    User_OPTIONAL_age = 1,
    User_OPTIONAL_phone = 2
} User_optional_field_t;

// 视图结构（不变）
typedef struct {
    const uint8_t *data;
    size_t size;
} User_view_t;

// 构建器结构（添加位图）
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint8_t presence_bitmap[User_PRESENCE_BITMAP_SIZE];
} User_builder_t;

// ----------------------------------------------------------------------------
// 位图辅助函数
// ----------------------------------------------------------------------------

static inline bool User_has_optional_field(const User_view_t *view, 
                                           User_optional_field_t field) {
    const uint8_t *bitmap = view->data;  // 位图在消息开头
    uint8_t byte_index = field / 8;
    uint8_t bit_index = field % 8;
    return (bitmap[byte_index] & (1 << bit_index)) != 0;
}

static inline void User_set_optional_field(User_builder_t *builder,
                                          User_optional_field_t field) {
    uint8_t byte_index = field / 8;
    uint8_t bit_index = field % 8;
    builder->presence_bitmap[byte_index] |= (1 << bit_index);
}

static inline void User_clear_optional_field(User_builder_t *builder,
                                            User_optional_field_t field) {
    uint8_t byte_index = field / 8;
    uint8_t bit_index = field % 8;
    builder->presence_bitmap[byte_index] &= ~(1 << bit_index);
}

// ----------------------------------------------------------------------------
// 必需字段访问器（不变）
// ----------------------------------------------------------------------------

static inline uint32_t User_id_get(const User_view_t *view) {
    return tbe_wire_read_u32(view->data + User_PRESENCE_BITMAP_SIZE, 
                            UserService_WIRE_BIG_ENDIAN);
}

static inline bool User_id_set(User_builder_t *builder, uint32_t value) {
    if (builder->size < User_PRESENCE_BITMAP_SIZE + 4) {
        return false;
    }
    tbe_wire_write_u32(builder->data + User_PRESENCE_BITMAP_SIZE, 
                      UserService_WIRE_BIG_ENDIAN, value);
    return true;
}

// username是必需的变长字段
static inline bool User_username_get(const User_view_t *view, tbe_var_data_t *out) {
    // 计算偏移（跳过位图、id）
    size_t offset = User_PRESENCE_BITMAP_SIZE + 4;
    return tbe_wire_read_var_data(view->data + offset, 
                                  view->size - offset,
                                  UserService_WIRE_BIG_ENDIAN, out);
}

// ----------------------------------------------------------------------------
// 可选字段访问器（新增检查）
// ----------------------------------------------------------------------------

static inline bool User_has_email(const User_view_t *view) {
    return User_has_optional_field(view, User_OPTIONAL_email);
}

static inline bool User_email_get(const User_view_t *view, tbe_var_data_t *out) {
    if (!User_has_email(view)) {
        out->data = NULL;
        out->size = 0;
        return false;
    }
    
    // 计算偏移：位图 + id + username
    size_t offset = User_PRESENCE_BITMAP_SIZE + 4;
    
    // 跳过username
    tbe_var_data_t username;
    if (!User_username_get(view, &username)) {
        return false;
    }
    offset += 4 + username.size;  // 4字节长度 + 数据
    
    return tbe_wire_read_var_data(view->data + offset,
                                  view->size - offset,
                                  UserService_WIRE_BIG_ENDIAN, out);
}

static inline bool User_email_set(User_builder_t *builder, 
                                  const char *value, size_t len) {
    // 标记字段存在
    User_set_optional_field(builder, User_OPTIONAL_email);
    
    // 计算写入位置
    size_t offset = /* 计算当前位置 */;
    
    return tbe_wire_write_var_data(builder->data + offset,
                                   builder->size - offset,
                                   UserService_WIRE_BIG_ENDIAN,
                                   value, len);
}

static inline void User_email_clear(User_builder_t *builder) {
    User_clear_optional_field(builder, User_OPTIONAL_email);
}

// age是可选的固定字段
static inline bool User_has_age(const User_view_t *view) {
    return User_has_optional_field(view, User_OPTIONAL_age);
}

static inline bool User_age_get(const User_view_t *view, uint32_t *out) {
    if (!User_has_age(view)) {
        return false;
    }
    
    // 计算偏移...
    size_t offset = /* ... */;
    *out = tbe_wire_read_u32(view->data + offset, UserService_WIRE_BIG_ENDIAN);
    return true;
}

static inline uint32_t User_age_get_or(const User_view_t *view, uint32_t default_value) {
    uint32_t value;
    return User_age_get(view, &value) ? value : default_value;
}

static inline bool User_age_set(User_builder_t *builder, uint32_t value) {
    User_set_optional_field(builder, User_OPTIONAL_age);
    // 写入逻辑...
    return true;
}

// ----------------------------------------------------------------------------
// 构建器初始化
// ----------------------------------------------------------------------------

static inline void User_builder_init(User_builder_t *builder, 
                                     uint8_t *buffer, size_t capacity) {
    builder->data = buffer;
    builder->size = 0;
    builder->capacity = capacity;
    memset(builder->presence_bitmap, 0, User_PRESENCE_BITMAP_SIZE);
    
    // 写入位图到buffer
    memcpy(buffer, builder->presence_bitmap, User_PRESENCE_BITMAP_SIZE);
    builder->size = User_PRESENCE_BITMAP_SIZE;
}

static inline bool User_builder_finalize(User_builder_t *builder, User_view_t *view) {
    // 将最终的位图写入buffer
    memcpy(builder->data, builder->presence_bitmap, User_PRESENCE_BITMAP_SIZE);
    
    view->data = builder->data;
    view->size = builder->size;
    return true;
}
```

### 使用示例

```c
// 创建消息
uint8_t buffer[1024];
User_builder_t builder;
User_builder_init(&builder, buffer, sizeof(buffer));

// 设置必需字段
User_id_set(&builder, 12345);
User_username_set(&builder, "john_doe", 8);

// 设置可选字段
if (has_email) {
    User_email_set(&builder, "john@example.com", 16);
}

if (has_age) {
    User_age_set(&builder, 30);
}

// 完成构建
User_view_t view;
User_builder_finalize(&builder, &view);

// 读取消息
uint32_t id = User_id_get(&view);

tbe_var_data_t username;
User_username_get(&view, &username);

if (User_has_email(&view)) {
    tbe_var_data_t email;
    User_email_get(&view, &email);
    printf("Email: %.*s\n", (int)email.size, email.data);
}

// 使用默认值
uint32_t age = User_age_get_or(&view, 18);  // 如果不存在，返回18
```

## 二进制布局

### 没有可选字段的情况（原TBE）

```
[id:4][username_len:4][username_data:N]
```

### 有可选字段的情况（新TBE）

```
[presence_bitmap:1][id:4][username_len:4][username_data:N][email_len:4][email_data:M][age:4][phone_len:4][phone_data:P]
 ^^^^^^^^^^^^^^^^^
 位0: email存在?
 位1: age存在?
 位2: phone存在?
```

## 性能分析

### 读取性能

```c
// 必需字段：无变化
uint32_t id = User_id_get(&view);
// 汇编：1条MOV指令 + 字节序转换

// 可选字段：增加1次位测试
if (User_has_email(&view)) {
    tbe_var_data_t email;
    User_email_get(&view, &email);
}
// 汇编：
//   TEST byte [addr], 0x01  ; 测试位 (~1纳秒)
//   JZ skip                 ; 跳转
//   CALL read_email         ; 读取数据
```

### 空间开销

```
消息数量 | 位图大小
---------|--------
1-8      | 1 字节
9-16     | 2 字节
17-24    | 3 字节
...      | ...
57-64    | 8 字节
```

### 对比测试结果（预期）

```
基准测试：1000万次消息编解码

原TBE（无可选字段）：
  编码：150ms
  解码：120ms
  总计：270ms

新TBE（3个可选字段）：
  编码：155ms (+3.3%)
  解码：125ms (+4.2%)
  总计：280ms (+3.7%)

结论：性能影响可接受（< 5%）
```

## 向后兼容性

### 场景1：旧代码读取新消息

```c
// 旧版本代码（不知道位图）
uint32_t id = tbe_wire_read_u32(view->data, endian);
// 问题：会读取到位图的第一个字节！

// 解决方案：版本号
if (schema_version >= 2) {
    // 跳过位图
    offset += presence_bitmap_size;
}
uint32_t id = tbe_wire_read_u32(view->data + offset, endian);
```

### 场景2：新代码读取旧消息

```c
// 新代码检测版本
if (schema_version < 2) {
    // 旧格式：假设所有可选字段都存在
    return true;
}

// 新格式：检查位图
return User_has_optional_field(view, field);
```

## 实施检查清单

- [ ] 更新lexer添加REQUIRED/OPTIONAL关键字
- [ ] 更新parser支持字段限定符
- [ ] 修改annotate_field函数标记可选字段
- [ ] 生成位图相关代码
- [ ] 修改访问器生成逻辑
- [ ] 添加has_XXX检查函数
- [ ] 添加XXX_get_or默认值函数
- [ ] 更新构建器添加位图管理
- [ ] 实现向后兼容性检查
- [ ] 添加单元测试
- [ ] 性能基准测试
- [ ] 更新文档和示例
- [ ] 更新Mustache模板