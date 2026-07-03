/**
 * @file data_bind.c
 * @brief Direct MIR runtime binary codec
 */

#include "data_bind.h"
#include "mir-gen.h"
#include "mir.h"
#include "node_tree.h"
#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "tbe_wire.h"
#include "turbo_fs.h"
#include "turbo_parser.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fsm/fsm.h>
#include <re/re.h>

extern char *tbe_read_varstring(const uint8_t *buf, size_t offset, size_t buf_remaining);

typedef struct {
  const char *name;
  int size;
  MIR_type_t mir_type;
  unsigned char is_float : 1;
  unsigned char is_64 : 1;
} type_meta_t;
static const type_meta_t TYPE_METAS[] = {
    {"uint8_t", 1, MIR_T_U8, 0, 0},   {"uint8", 1, MIR_T_U8, 0, 0},
    {"u8", 1, MIR_T_U8, 0, 0},        {"byte", 1, MIR_T_U8, 0, 0},
    {"int8_t", 1, MIR_T_I8, 0, 0},    {"int8", 1, MIR_T_I8, 0, 0},
    {"uint16_t", 2, MIR_T_U16, 0, 0}, {"uint16", 2, MIR_T_U16, 0, 0},
    {"u16", 2, MIR_T_U16, 0, 0},      {"int16_t", 2, MIR_T_I16, 0, 0},
    {"int16", 2, MIR_T_I16, 0, 0},    {"uint32_t", 4, MIR_T_U32, 0, 0},
    {"uint32", 4, MIR_T_U32, 0, 0},   {"u32", 4, MIR_T_U32, 0, 0},
    {"int32_t", 4, MIR_T_I32, 0, 0},  {"int32", 4, MIR_T_I32, 0, 0},
    {"i32", 4, MIR_T_I32, 0, 0},      {"uint64_t", 8, MIR_T_U64, 0, 1},
    {"uint64", 8, MIR_T_U64, 0, 1},   {"u64", 8, MIR_T_U64, 0, 1},
    {"int64_t", 8, MIR_T_I64, 0, 1},  {"int64", 8, MIR_T_I64, 0, 1},
    {"float", 4, MIR_T_F, 1, 0},      {"f32", 4, MIR_T_F, 1, 0},
    {"double", 8, MIR_T_D, 1, 0},     {"f64", 8, MIR_T_D, 1, 0},
    {"bool", 1, MIR_T_U8, 0, 0},      {NULL, 0, MIR_T_UNDEF, 0, 0}};

typedef struct owned_alloc_node {
  void *ptr;
  struct owned_alloc_node *next;
} owned_alloc_node_t;
typedef struct mir_func_node {
  char *type_name;
  void *parse_fn;
  struct mir_func_node *next;
} mir_func_node_t;

typedef struct mir_cache_entry {
  char schema_hash[65];
  MIR_context_t shared_ctx;
  mir_func_node_t *func_head;
  int ref_count;
  struct mir_cache_entry *next;
} mir_cache_entry_t;

/* Small object pool for frequently allocated DataBindValue nodes */
#define VALUE_POOL_SIZE 64
#define VALUE_POOL_ENABLED 1

typedef struct value_pool {
  DataBindValue *free_list;
  size_t allocated_count;
  size_t reused_count;
} value_pool_t;

static value_pool_t g_value_pool = {NULL, 0, 0};
static int g_value_pool_enabled = VALUE_POOL_ENABLED;

static mir_cache_entry_t *g_mir_cache_head = NULL;
static int g_mir_cache_enabled = 1;

typedef struct data_bind_runtime_api {
  DataBindValue *(*create_object)(void);
  void (*set_field_int)(DataBindValue *obj, const char *name, int32_t val);
  void (*set_field_int64)(DataBindValue *obj, const char *name, int64_t val);
  void (*set_field_double)(DataBindValue *obj, const char *name, double val);
  void (*set_field_bool)(DataBindValue *obj, const char *name, int val);
  void (*set_field_string)(DataBindValue *obj, const char *name, const char *val);
  void (*set_field_bytes)(DataBindValue *obj, const char *name, const uint8_t *data, size_t len);
  void (*set_field_uuid)(DataBindValue *obj, const char *name, const uint8_t *data);
  DataBindValue *(*create_list)(void);
  void (*add_list_item_int)(DataBindValue *list, int32_t val);
  void (*add_list_item_int64)(DataBindValue *list, int64_t val);
  void (*add_list_item_double)(DataBindValue *list, double val);
  void (*add_list_item_bool)(DataBindValue *list, int val);
  void (*add_list_item_string)(DataBindValue *list, const char *val);
  void (*add_list_item_object)(DataBindValue *list, DataBindValue *obj);
  void (*set_field_list)(DataBindValue *obj, const char *name, DataBindValue *list);
  void (*set_field_object)(DataBindValue *obj, const char *name, DataBindValue *child);
  DataBindValue *(*create_set)(void);
  void (*add_set_item_int)(DataBindValue *set, int32_t val);
  void (*add_set_item_double)(DataBindValue *set, double val);
  void (*add_set_item_bool)(DataBindValue *set, int val);
  void (*add_set_item_string)(DataBindValue *set, const char *val);
  void (*set_field_set)(DataBindValue *obj, const char *name, DataBindValue *set);
  DataBindValue *(*create_map)(void);
  void (*add_map_entry_string_string)(DataBindValue *map, const char *key, const char *val);
  void (*add_map_entry_string_int)(DataBindValue *map, const char *key, int32_t val);
  void (*add_map_entry_string_double)(DataBindValue *map, const char *key, double val);
  void (*add_map_entry_string_bool)(DataBindValue *map, const char *key, int val);
  void (*set_field_map)(DataBindValue *obj, const char *name, DataBindValue *map);
} data_bind_runtime_api_t;

struct DataBind {
  MIR_context_t ctx;
  Node *schema_root;
  mir_func_node_t *func_head;
  owned_alloc_node_t *owned_allocs;
  char error[256];
  char binary_error[256];
  data_bind_runtime_api_t api;
  char schema_hash[65];  /* SHA-256 hash of schema for caching */
  int is_cloned;         /* Whether this codec shares MIR context with another */
};

typedef struct data_bind_value_field {
  char *name;
  DataBindValue *value;
} data_bind_value_field_t;

typedef struct data_bind_value_array {
  DataBindValue **items;
  size_t count;
  size_t capacity;
} data_bind_value_array_t;

typedef struct data_bind_value_field_array {
  data_bind_value_field_t *items;
  size_t count;
  size_t capacity;
} data_bind_value_field_array_t;

typedef struct data_bind_value_map_entry {
  char *key;
  DataBindValue *value;
} data_bind_value_map_entry_t;

typedef struct data_bind_value_map_array {
  data_bind_value_map_entry_t *items;
  size_t count;
  size_t capacity;
} data_bind_value_map_array_t;

struct DataBindValue {
  DataBindValueKind kind;
  union {
    int32_t int_val;
    int64_t int64_val;
    double double_val;
    int bool_val;
    struct {
      char *ptr;
    } string_val;
    struct {
      uint8_t *ptr;
      size_t len;
    } bytes_val;
    uuid_t uuid_val;
    turbo_datetime_t datetime_val;
    DataBindDate date_val;
    DataBindTime time_val;
    int64_t duration_ms;
    DataBindDecimal decimal_val;
    struct {
      char *ptr;
    } bigint_val;
    DataBindMoney money_val;
    data_bind_value_field_array_t object_val;
    data_bind_value_array_t array_val;
    data_bind_value_map_array_t map_val;
  } data;
};

typedef enum {
  EF_INT,
  EF_I64,
  EF_DBL,
  EF_BOOL,
  EF_UUID,
  EF_STR,
  EF_FIX_BYTES,
  EF_VAR_BYTES,
  EF_LIST_INT,
  EF_LIST_I64,
  EF_LIST_DBL,
  EF_LIST_BOOL,
  EF_LIST_STR,
  EF_LIST_OBJ,
  EF_SET_INT,
  EF_SET_DBL,
  EF_SET_BOOL,
  EF_SET_STR,
  EF_MAP_STR_STR,
  EF_MAP_STR_INT,
  EF_MAP_STR_DBL,
  EF_MAP_STR_BOOL,
  EF_GROUP
} emit_kind_t;

typedef struct emit_field emit_field_t;
typedef struct {
  emit_field_t *items;
  size_t count;
  size_t capacity;
} emit_field_array_t;
struct emit_field {
  char *name;
  emit_kind_t kind;
  int size;
  MIR_type_t mir_type;
  unsigned char is_float : 1;
  unsigned char is_64 : 1;
  unsigned char has_set_bytes : 1;
  size_t fixed_count;
  int group_dim;
  emit_field_array_t children;
};

typedef struct {
  MIR_item_t import_item;
  MIR_item_t proto_item;
} external_ref_t;
typedef struct {
  external_ref_t create_obj, set_int, set_i64, set_dbl, set_bool, set_str, set_bytes;
  external_ref_t set_uuid;
  external_ref_t create_list, add_list_int, add_list_i64, add_list_dbl, add_list_bool,
      add_list_str, add_list_obj, set_list;
  external_ref_t create_set, add_set_int, add_set_dbl, add_set_bool, add_set_str, set_set;
  external_ref_t create_map, add_map_str_str, add_map_str_int, add_map_str_dbl,
      add_map_str_bool, set_map;
  external_ref_t read_varstr, free_fn;
} external_items_t;

typedef struct {
  DataBind *codec;
  MIR_context_t ctx;
  MIR_module_t module;
  external_items_t ext;
  size_t temp_name_id;
  size_t data_name_id;
} mir_builder_t;
typedef struct {
  mir_builder_t *builder;
  MIR_item_t func_item;
  MIR_func_t func;
  MIR_reg_t buf_reg;
  MIR_reg_t len_reg;
  MIR_reg_t off_reg;
  MIR_reg_t obj_reg;
  MIR_label_t fail_label;
} mir_emitter_t;

static const type_meta_t *find_type_meta(const char *type) {
  const type_meta_t *m;
  if (type == NULL) return NULL;
  for (m = TYPE_METAS; m->name != NULL; m++)
    if (strcmp(type, m->name) == 0) return m;
  return NULL;
}

static void set_i64_noop(DataBindValue *o, const char *n, int64_t v) {
  (void)o;
  (void)n;
  (void)v;
}
static void set_bytes_noop(DataBindValue *o, const char *n, const uint8_t *d, size_t l) {
  (void)o;
  (void)n;
  (void)d;
  (void)l;
}
static void set_uuid_noop(DataBindValue *o, const char *n, const uint8_t *d) {
  (void)o;
  (void)n;
  (void)d;
}
static void set_i32_noop(DataBindValue *o, const char *n, int32_t v) {
  (void)o;
  (void)n;
  (void)v;
}
static void set_dbl_noop(DataBindValue *o, const char *n, double v) {
  (void)o;
  (void)n;
  (void)v;
}
static void set_bool_noop(DataBindValue *o, const char *n, int v) {
  (void)o;
  (void)n;
  (void)v;
}
static void set_str_noop(DataBindValue *o, const char *n, const char *v) {
  (void)o;
  (void)n;
  (void)v;
}

static char *dbv_strdup(const char *src) {
  size_t len;
  char *dst;
  if (src == NULL) return NULL;
  len = strlen(src) + 1;
  dst = (char *)malloc(len);
  if (dst == NULL) return NULL;
  memcpy(dst, src, len);
  return dst;
}

static DataBindValue *dbv_new(DataBindValueKind kind) {
  DataBindValue *value = NULL;
  
  /* Try to get from pool first if enabled */
  if (g_value_pool_enabled && g_value_pool.free_list != NULL) {
    value = g_value_pool.free_list;
    g_value_pool.free_list = (DataBindValue *)value->data.object_val.items;
    g_value_pool.reused_count++;
    memset(value, 0, sizeof(*value));
  } else {
    value = (DataBindValue *)calloc(1, sizeof(*value));
    if (g_value_pool_enabled && value != NULL) {
      g_value_pool.allocated_count++;
    }
  }
  
  if (value != NULL) value->kind = kind;
  return value;
}

static int dbv_array_push(data_bind_value_array_t *array, DataBindValue *value) {
  DataBindValue **items;
  size_t capacity;
  if (array == NULL || value == NULL) return 0;
  if (array->count == array->capacity) {
    capacity = array->capacity == 0 ? 8 : array->capacity * 2;
    items = (DataBindValue **)realloc(array->items, capacity * sizeof(*items));
    if (items == NULL) return 0;
    array->items = items;
    array->capacity = capacity;
  }
  array->items[array->count++] = value;
  return 1;
}

static int dbv_object_set(DataBindValue *obj, const char *name, DataBindValue *value) {
  data_bind_value_field_array_t *fields;
  data_bind_value_field_t *items;
  size_t capacity;
  if (obj == NULL || obj->kind != DATA_BIND_VALUE_OBJECT || name == NULL || value == NULL)
    return 0;
  fields = &obj->data.object_val;
  if (fields->count == fields->capacity) {
    capacity = fields->capacity == 0 ? 8 : fields->capacity * 2;
    items = (data_bind_value_field_t *)realloc(fields->items, capacity * sizeof(*items));
    if (items == NULL) return 0;
    fields->items = items;
    fields->capacity = capacity;
  }
  fields->items[fields->count].name = dbv_strdup(name);
  if (fields->items[fields->count].name == NULL) return 0;
  fields->items[fields->count].value = value;
  fields->count++;
  return 1;
}

static int dbv_map_set(DataBindValue *map, const char *key, DataBindValue *value) {
  data_bind_value_map_array_t *entries;
  data_bind_value_map_entry_t *items;
  size_t capacity;
  if (map == NULL || map->kind != DATA_BIND_VALUE_MAP || key == NULL || value == NULL) return 0;
  entries = &map->data.map_val;
  if (entries->count == entries->capacity) {
    capacity = entries->capacity == 0 ? 8 : entries->capacity * 2;
    items = (data_bind_value_map_entry_t *)realloc(entries->items, capacity * sizeof(*items));
    if (items == NULL) return 0;
    entries->items = items;
    entries->capacity = capacity;
  }
  entries->items[entries->count].key = dbv_strdup(key);
  if (entries->items[entries->count].key == NULL) return 0;
  entries->items[entries->count].value = value;
  entries->count++;
  return 1;
}

static int dbv_map_has_key(const DataBindValue *map, const char *key) {
  size_t i;
  if (map == NULL || map->kind != DATA_BIND_VALUE_MAP || key == NULL) return 0;
  for (i = 0; i < map->data.map_val.count; i++) {
    if (map->data.map_val.items[i].key != NULL &&
        strcmp(map->data.map_val.items[i].key, key) == 0)
      return 1;
  }
  return 0;
}

void data_bind_value_free(DataBindValue *value) {
  size_t i;
  if (value == NULL) return;
  switch (value->kind) {
  case DATA_BIND_VALUE_OBJECT:
    for (i = 0; i < value->data.object_val.count; i++) {
      free(value->data.object_val.items[i].name);
      data_bind_value_free(value->data.object_val.items[i].value);
    }
    free(value->data.object_val.items);
    break;
  case DATA_BIND_VALUE_LIST:
  case DATA_BIND_VALUE_SET:
    for (i = 0; i < value->data.array_val.count; i++)
      data_bind_value_free(value->data.array_val.items[i]);
    free(value->data.array_val.items);
    break;
  case DATA_BIND_VALUE_MAP:
    for (i = 0; i < value->data.map_val.count; i++) {
      free(value->data.map_val.items[i].key);
      data_bind_value_free(value->data.map_val.items[i].value);
    }
    free(value->data.map_val.items);
    break;
  case DATA_BIND_VALUE_STRING:
    free(value->data.string_val.ptr);
    break;
  case DATA_BIND_VALUE_BIGINT:
    free(value->data.bigint_val.ptr);
    break;
  case DATA_BIND_VALUE_BYTES:
    free(value->data.bytes_val.ptr);
    break;
  default:
    break;
  }
  
  /* Return to pool if enabled and pool not full */
  if (g_value_pool_enabled && g_value_pool.allocated_count < VALUE_POOL_SIZE) {
    /* Reuse object_val.items pointer as next pointer in free list */
    value->data.object_val.items = (data_bind_value_field_t *)g_value_pool.free_list;
    g_value_pool.free_list = value;
  } else {
    free(value);
  }
}

static DataBindValue *dbv_int(int32_t value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_INT);
  if (v != NULL) v->data.int_val = value;
  return v;
}

static DataBindValue *dbv_int64(int64_t value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_INT64);
  if (v != NULL) v->data.int64_val = value;
  return v;
}

static DataBindValue *dbv_double(double value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_DOUBLE);
  if (v != NULL) v->data.double_val = value;
  return v;
}

static DataBindValue *dbv_bool(int value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_BOOL);
  if (v != NULL) v->data.bool_val = value != 0;
  return v;
}

static DataBindValue *dbv_string(const char *value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_STRING);
  if (v == NULL) return NULL;
  v->data.string_val.ptr = dbv_strdup(value != NULL ? value : "");
  if (v->data.string_val.ptr == NULL) {
    data_bind_value_free(v);
    return NULL;
  }
  return v;
}

static DataBindValue *dbv_bytes(const uint8_t *data, size_t len) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_BYTES);
  if (v == NULL) return NULL;
  if (len > 0) {
    v->data.bytes_val.ptr = (uint8_t *)malloc(len);
    if (v->data.bytes_val.ptr == NULL) {
      data_bind_value_free(v);
      return NULL;
    }
    if (data != NULL) memcpy(v->data.bytes_val.ptr, data, len);
  }
  v->data.bytes_val.len = len;
  return v;
}

static DataBindValue *dbv_uuid_bytes(const uint8_t *data) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_UUID);
  if (v == NULL || data == NULL) {
    data_bind_value_free(v);
    return NULL;
  }
  memcpy(v->data.uuid_val.bytes, data, sizeof(v->data.uuid_val.bytes));
  return v;
}

static DataBindValue *dbv_uuid_text(const char *text) {
  uuid_t uuid;
  if (text == NULL || !uuid_from_s(text, &uuid)) return NULL;
  return dbv_uuid_bytes(uuid.bytes);
}

static DataBindValue *dbv_datetime(turbo_datetime_t value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_DATETIME);
  if (v == NULL) return NULL;
  v->data.datetime_val = value;
  return v;
}

static DataBindValue *dbv_datetime_text(const char *text) {
  turbo_datetime_t dt;
  if (text == NULL || turbo_parse_datetime(text, strlen(text), &dt) != 0) return NULL;
  return dbv_datetime(dt);
}

static int db_date_valid(int year, int month, int day) {
  static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days;
  if (year < 1 || month < 1 || month > 12 || day < 1) return 0;
  days = days_per_month[month - 1];
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days = 29;
  return day <= days;
}

static int db_parse_date_text(const char *text, DataBindDate *out) {
  turbo_datetime_t dt;
  int year = 0, month = 0, day = 0, consumed = 0;
  if (text == NULL || out == NULL) return 0;
  if (sscanf(text, "%d-%d-%d%n", &year, &month, &day, &consumed) == 3 ||
      sscanf(text, "%d/%d/%d%n", &year, &month, &day, &consumed) == 3) {
    if (text[consumed] != '\0' || !db_date_valid(year, month, day)) return 0;
    out->year = year;
    out->month = month;
    out->day = day;
    return 1;
  }
  if (turbo_parse_datetime(text, strlen(text), &dt) == 0) {
    out->year = dt.year;
    out->month = dt.month;
    out->day = dt.day;
    return 1;
  }
  return 0;
}

static int db_parse_time_text(const char *text, DataBindTime *out) {
  int hour = 0, minute = 0, second = 0, millisecond = 0, consumed = 0;
  if (text == NULL || out == NULL) return 0;
  if (sscanf(text, "%d:%d:%d.%d%n", &hour, &minute, &second, &millisecond, &consumed) >= 3 ||
      sscanf(text, "%d:%d:%d%n", &hour, &minute, &second, &consumed) >= 3 ||
      sscanf(text, "%d:%d%n", &hour, &minute, &consumed) >= 2) {
    if (text[consumed] == '\0' && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 &&
        second >= 0 && second <= 60 && millisecond >= 0 && millisecond <= 999) {
      out->hour = hour;
      out->minute = minute;
      out->second = second;
      out->millisecond = millisecond;
      return 1;
    }
  }
  return 0;
}

static int db_parse_duration_text(const char *text, int64_t *out) {
  const char *p;
  int sign = 1;
  double total = 0.0;
  int saw_value = 0;
  long long hours = 0, minutes = 0, seconds = 0, millis = 0;
  int consumed = 0;
  int colon_sign;
  if (text == NULL || out == NULL) return 0;
  if (sscanf(text, "%lld:%lld:%lld.%lld%n", &hours, &minutes, &seconds, &millis, &consumed) ==
          4 &&
      text[consumed] == '\0' && minutes >= 0 && minutes <= 59 && seconds >= 0 &&
      seconds <= 60 && millis >= 0 && millis <= 999) {
    colon_sign = hours < 0 ? -1 : 1;
    if (hours < 0) hours = -hours;
    *out = (int64_t)(colon_sign *
                     (hours * 3600000LL + minutes * 60000LL + seconds * 1000LL + millis));
    return 1;
  }
  if (sscanf(text, "%lld:%lld:%lld%n", &hours, &minutes, &seconds, &consumed) == 3 &&
      text[consumed] == '\0' && minutes >= 0 && minutes <= 59 && seconds >= 0 &&
      seconds <= 60) {
    colon_sign = hours < 0 ? -1 : 1;
    if (hours < 0) hours = -hours;
    *out = (int64_t)(colon_sign * (hours * 3600000LL + minutes * 60000LL + seconds * 1000LL));
    return 1;
  }
  p = text;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '-') {
    sign = -1;
    p++;
  } else if (*p == '+') {
    p++;
  }
  while (*p != '\0') {
    char *next = NULL;
    double n;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '\0') break;
    errno = 0;
    n = strtod(p, &next);
    if (errno != 0 || next == p) return 0;
    p = next;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '\0') {
      total += n;
      saw_value = 1;
      break;
    }
    if (p[0] == 'm' && p[1] == 's') {
      total += n;
      p += 2;
    } else if (*p == 's') {
      total += n * 1000.0;
      p++;
    } else if (*p == 'm') {
      total += n * 60000.0;
      p++;
    } else if (*p == 'h') {
      total += n * 3600000.0;
      p++;
    } else if (*p == 'd') {
      total += n * 86400000.0;
      p++;
    } else {
      return 0;
    }
    saw_value = 1;
  }
  if (!saw_value || total > (double)INT64_MAX) return 0;
  *out = (int64_t)(sign * total);
  return 1;
}

static int db_date_to_text(DataBindDate date, char *out, size_t len) {
  return out != NULL && len > 0 && db_date_valid(date.year, date.month, date.day) &&
         snprintf(out, len, "%04d-%02d-%02d", date.year, date.month, date.day) > 0;
}

static int db_time_to_text(DataBindTime time, char *out, size_t len) {
  if (out == NULL || len == 0 || time.hour < 0 || time.hour > 23 ||
      time.minute < 0 || time.minute > 59 || time.second < 0 || time.second > 60 ||
      time.millisecond < 0 || time.millisecond > 999)
    return 0;
  if (time.millisecond > 0)
    return snprintf(out, len, "%02d:%02d:%02d.%03d", time.hour, time.minute,
                    time.second, time.millisecond) > 0;
  return snprintf(out, len, "%02d:%02d:%02d", time.hour, time.minute, time.second) > 0;
}

static int db_duration_to_text(int64_t ms, char *out, size_t len) {
  int64_t rem;
  int64_t hours;
  int64_t minutes;
  int64_t seconds;
  if (out == NULL || len == 0) return 0;
  rem = ms < 0 ? -ms : ms;
  hours = rem / 3600000;
  rem %= 3600000;
  minutes = rem / 60000;
  rem %= 60000;
  seconds = rem / 1000;
  rem %= 1000;
  return snprintf(out, len, "%s%lld:%02lld:%02lld.%03lld", ms < 0 ? "-" : "",
                  (long long)hours, (long long)minutes, (long long)seconds,
                  (long long)rem) > 0;
}

static DataBindValue *dbv_date(DataBindDate value) {
  DataBindValue *v;
  if (!db_date_valid(value.year, value.month, value.day)) return NULL;
  v = dbv_new(DATA_BIND_VALUE_DATE);
  if (v != NULL) v->data.date_val = value;
  return v;
}

static DataBindValue *dbv_date_text(const char *text) {
  DataBindDate date;
  if (!db_parse_date_text(text, &date)) return NULL;
  return dbv_date(date);
}

static DataBindValue *dbv_time(DataBindTime value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_TIME);
  if (v != NULL) v->data.time_val = value;
  return v;
}

static DataBindValue *dbv_time_text(const char *text) {
  DataBindTime time;
  if (!db_parse_time_text(text, &time)) return NULL;
  return dbv_time(time);
}

static DataBindValue *dbv_duration(int64_t value) {
  DataBindValue *v = dbv_new(DATA_BIND_VALUE_DURATION);
  if (v != NULL) v->data.duration_ms = value;
  return v;
}

static DataBindValue *dbv_duration_text(const char *text) {
  int64_t ms = 0;
  if (!db_parse_duration_text(text, &ms)) return NULL;
  return dbv_duration(ms);
}

static int db_decimal_normalize(DataBindDecimal *value) {
  if (value == NULL) return 0;
  if (value->scale < 0) return 0;
  while (value->scale > 0 && value->mantissa % 10 == 0) {
    value->mantissa /= 10;
    value->scale--;
  }
  if (value->mantissa == 0) value->scale = 0;
  return 1;
}

static int db_parse_decimal_text(const char *text, DataBindDecimal *out) {
  const char *p;
  int sign = 1;
  int saw_digit = 0;
  int saw_dot = 0;
  int32_t scale = 0;
  uint64_t acc = 0;
  uint64_t limit;

  if (text == NULL || out == NULL) return 0;
  p = text;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '-') {
    sign = -1;
    p++;
  } else if (*p == '+') {
    p++;
  }
  limit = sign < 0 ? (uint64_t)INT64_MAX + 1ULL : (uint64_t)INT64_MAX;
  while (*p != '\0') {
    if (isdigit((unsigned char)*p)) {
      unsigned digit = (unsigned)(*p - '0');
      if (acc > (limit - digit) / 10ULL) return 0;
      acc = acc * 10ULL + digit;
      saw_digit = 1;
      if (saw_dot) {
        if (scale == INT32_MAX) return 0;
        scale++;
      }
      p++;
      continue;
    }
    if (*p == '.') {
      if (saw_dot) return 0;
      saw_dot = 1;
      p++;
      continue;
    }
    if (isspace((unsigned char)*p)) {
      while (isspace((unsigned char)*p)) p++;
      if (*p == '\0') break;
    }
    return 0;
  }
  if (!saw_digit) return 0;
  if (sign < 0) {
    out->mantissa = acc == (uint64_t)INT64_MAX + 1ULL ? INT64_MIN : -(int64_t)acc;
  } else {
    out->mantissa = (int64_t)acc;
  }
  out->scale = scale;
  return db_decimal_normalize(out);
}

static int db_decimal_to_text(DataBindDecimal value, char *out, size_t len) {
  char digits[32];
  char *p = digits + sizeof(digits);
  uint64_t mag;
  size_t digit_count;
  size_t pos = 0;
  int negative;

  if (out == NULL || len == 0 || value.scale < 0) return 0;
  db_decimal_normalize(&value);
  negative = value.mantissa < 0;
  mag = negative ? (uint64_t)(-(value.mantissa + 1)) + 1ULL : (uint64_t)value.mantissa;
  *--p = '\0';
  do {
    *--p = (char)('0' + (mag % 10ULL));
    mag /= 10ULL;
  } while (mag != 0);
  digit_count = strlen(p);

  if (negative) {
    if (pos + 1 >= len) return 0;
    out[pos++] = '-';
  }

  if (value.scale == 0) {
    if (pos + digit_count >= len) return 0;
    memcpy(out + pos, p, digit_count + 1);
    return 1;
  }

  if ((size_t)value.scale >= digit_count) {
    size_t zeros = (size_t)value.scale - digit_count;
    if (pos + 2 + zeros + digit_count >= len) return 0;
    out[pos++] = '0';
    out[pos++] = '.';
    while (zeros-- > 0) out[pos++] = '0';
    memcpy(out + pos, p, digit_count);
    pos += digit_count;
    out[pos] = '\0';
    return 1;
  }

  {
    size_t whole = digit_count - (size_t)value.scale;
    if (pos + digit_count + 1 >= len) return 0;
    memcpy(out + pos, p, whole);
    pos += whole;
    out[pos++] = '.';
    memcpy(out + pos, p + whole, (size_t)value.scale);
    pos += (size_t)value.scale;
    out[pos] = '\0';
    return 1;
  }
}

static DataBindValue *dbv_decimal(DataBindDecimal value) {
  DataBindValue *v;
  if (!db_decimal_normalize(&value)) return NULL;
  v = dbv_new(DATA_BIND_VALUE_DECIMAL);
  if (v != NULL) v->data.decimal_val = value;
  return v;
}

static DataBindValue *dbv_decimal_text(const char *text) {
  DataBindDecimal value;
  if (!db_parse_decimal_text(text, &value)) return NULL;
  return dbv_decimal(value);
}

static int db_validate_currency(const char *text);

static int db_bigint_canonical_text(const char *text, char *out, size_t len) {
  const char *p;
  const char *digits;
  size_t digits_len;
  int negative = 0;
  if (text == NULL || out == NULL || len == 0) return 0;
  p = text;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '-') {
    negative = 1;
    p++;
  } else if (*p == '+') {
    p++;
  }
  digits = p;
  while (*p == '0') p++;
  if (!isdigit((unsigned char)*p)) {
    const char *q = digits;
    int saw_zero = 0;
    while (*q == '0') {
      saw_zero = 1;
      q++;
    }
    while (isspace((unsigned char)*q)) q++;
    if (!saw_zero || *q != '\0' || len < 2) return 0;
    memcpy(out, "0", 2);
    return 1;
  }
  digits = p;
  while (isdigit((unsigned char)*p)) p++;
  digits_len = (size_t)(p - digits);
  while (isspace((unsigned char)*p)) p++;
  if (*p != '\0' || digits_len == 0) return 0;
  if ((negative ? 1 : 0) + digits_len + 1 > len) return 0;
  if (negative) {
    out[0] = '-';
    memcpy(out + 1, digits, digits_len);
    out[digits_len + 1] = '\0';
  } else {
    memcpy(out, digits, digits_len);
    out[digits_len] = '\0';
  }
  return 1;
}

static DataBindValue *dbv_bigint_text(const char *text) {
  char stack_buf[256];
  char *canonical = stack_buf;
  size_t need;
  DataBindValue *v;
  if (text == NULL) return NULL;
  need = strlen(text) + 2;
  if (need > sizeof(stack_buf)) {
    canonical = (char *)malloc(need);
    if (canonical == NULL) return NULL;
  }
  if (!db_bigint_canonical_text(text, canonical, need)) {
    if (canonical != stack_buf) free(canonical);
    return NULL;
  }
  v = dbv_new(DATA_BIND_VALUE_BIGINT);
  if (v == NULL) {
    if (canonical != stack_buf) free(canonical);
    return NULL;
  }
  v->data.bigint_val.ptr = dbv_strdup(canonical);
  if (canonical != stack_buf) free(canonical);
  if (v->data.bigint_val.ptr == NULL) {
    data_bind_value_free(v);
    return NULL;
  }
  return v;
}

static int db_parse_money_text(const char *text, DataBindMoney *out) {
  char token_a[128];
  char token_b[128];
  char extra[2];
  DataBindDecimal amount;
  if (text == NULL || out == NULL) return 0;
  token_a[0] = token_b[0] = extra[0] = '\0';
  if (sscanf(text, " %127s %127s %1s", token_a, token_b, extra) != 2) return 0;
  if (db_validate_currency(token_a) && db_parse_decimal_text(token_b, &amount)) {
    out->amount = amount;
    memcpy(out->currency, token_a, 4);
    return 1;
  }
  if (db_parse_decimal_text(token_a, &amount) && db_validate_currency(token_b)) {
    out->amount = amount;
    memcpy(out->currency, token_b, 4);
    return 1;
  }
  return 0;
}

static int db_money_to_text(DataBindMoney value, char *out, size_t len) {
  char amount[64];
  if (out == NULL || len == 0 || !db_validate_currency(value.currency)) return 0;
  if (!db_decimal_to_text(value.amount, amount, sizeof(amount))) return 0;
  return snprintf(out, len, "%s %s", value.currency, amount) > 0;
}

static DataBindValue *dbv_money(DataBindMoney value) {
  DataBindValue *v;
  if (!db_validate_currency(value.currency) || !db_decimal_normalize(&value.amount)) return NULL;
  v = dbv_new(DATA_BIND_VALUE_MONEY);
  if (v != NULL) v->data.money_val = value;
  return v;
}

static DataBindValue *dbv_money_text(const char *text) {
  DataBindMoney value;
  if (!db_parse_money_text(text, &value)) return NULL;
  return dbv_money(value);
}

static DataBindValue *dynamic_create_object(void) { return (DataBindValue *)dbv_new(DATA_BIND_VALUE_OBJECT); }
static DataBindValue *dynamic_create_list(void) { return (DataBindValue *)dbv_new(DATA_BIND_VALUE_LIST); }
static DataBindValue *dynamic_create_set(void) { return (DataBindValue *)dbv_new(DATA_BIND_VALUE_SET); }
static DataBindValue *dynamic_create_map(void) { return (DataBindValue *)dbv_new(DATA_BIND_VALUE_MAP); }

static void dynamic_set_field_int(DataBindValue *obj, const char *name, int32_t val) {
  DataBindValue *child = dbv_int(val);
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, child)) data_bind_value_free(child);
}
static void dynamic_set_field_int64(DataBindValue *obj, const char *name, int64_t val) {
  DataBindValue *child = dbv_int64(val);
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, child)) data_bind_value_free(child);
}
static void dynamic_set_field_double(DataBindValue *obj, const char *name, double val) {
  DataBindValue *child = dbv_double(val);
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, child)) data_bind_value_free(child);
}
static void dynamic_set_field_bool(DataBindValue *obj, const char *name, int val) {
  DataBindValue *child = dbv_bool(val);
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, child)) data_bind_value_free(child);
}
static void dynamic_set_field_string(DataBindValue *obj, const char *name, const char *val) {
  DataBindValue *child = dbv_string(val);
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, child)) data_bind_value_free(child);
}
static void dynamic_set_field_bytes(DataBindValue *obj, const char *name, const uint8_t *data, size_t len) {
  DataBindValue *child = dbv_bytes(data, len);
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, child)) data_bind_value_free(child);
}
static void dynamic_set_field_uuid(DataBindValue *obj, const char *name, const uint8_t *data) {
  DataBindValue *child = dbv_uuid_bytes(data);
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, child)) data_bind_value_free(child);
}
static void dynamic_set_field_value(DataBindValue *obj, const char *name, DataBindValue *child) {
  if (child == NULL || !dbv_object_set((DataBindValue *)obj, name, (DataBindValue *)child))
    data_bind_value_free((DataBindValue *)child);
}
static void dynamic_add_list_item_int(DataBindValue *list, int32_t val) {
  DataBindValue *child = dbv_int(val);
  if (child == NULL || !dbv_array_push(&((DataBindValue *)list)->data.array_val, child))
    data_bind_value_free(child);
}
static void dynamic_add_list_item_int64(DataBindValue *list, int64_t val) {
  DataBindValue *child = dbv_int64(val);
  if (child == NULL || !dbv_array_push(&((DataBindValue *)list)->data.array_val, child))
    data_bind_value_free(child);
}
static void dynamic_add_list_item_double(DataBindValue *list, double val) {
  DataBindValue *child = dbv_double(val);
  if (child == NULL || !dbv_array_push(&((DataBindValue *)list)->data.array_val, child))
    data_bind_value_free(child);
}
static void dynamic_add_list_item_bool(DataBindValue *list, int val) {
  DataBindValue *child = dbv_bool(val);
  if (child == NULL || !dbv_array_push(&((DataBindValue *)list)->data.array_val, child))
    data_bind_value_free(child);
}
static void dynamic_add_list_item_string(DataBindValue *list, const char *val) {
  DataBindValue *child = dbv_string(val);
  if (child == NULL || !dbv_array_push(&((DataBindValue *)list)->data.array_val, child))
    data_bind_value_free(child);
}
static void dynamic_add_list_item_object(DataBindValue *list, DataBindValue *obj) {
  if (obj == NULL || !dbv_array_push(&((DataBindValue *)list)->data.array_val, (DataBindValue *)obj))
    data_bind_value_free((DataBindValue *)obj);
}
static void dynamic_add_map_entry_string_string(DataBindValue *map, const char *key, const char *val) {
  DataBindValue *child = dbv_string(val);
  if (child == NULL || !dbv_map_set((DataBindValue *)map, key, child)) data_bind_value_free(child);
}
static void dynamic_add_map_entry_string_int(DataBindValue *map, const char *key, int32_t val) {
  DataBindValue *child = dbv_int(val);
  if (child == NULL || !dbv_map_set((DataBindValue *)map, key, child)) data_bind_value_free(child);
}
static void dynamic_add_map_entry_string_double(DataBindValue *map, const char *key, double val) {
  DataBindValue *child = dbv_double(val);
  if (child == NULL || !dbv_map_set((DataBindValue *)map, key, child)) data_bind_value_free(child);
}
static void dynamic_add_map_entry_string_bool(DataBindValue *map, const char *key, int val) {
  DataBindValue *child = dbv_bool(val);
  if (child == NULL || !dbv_map_set((DataBindValue *)map, key, child)) data_bind_value_free(child);
}

static const data_bind_runtime_api_t DYNAMIC_VALUE_API = {
    .create_object = dynamic_create_object,
    .set_field_int = dynamic_set_field_int,
    .set_field_int64 = dynamic_set_field_int64,
    .set_field_double = dynamic_set_field_double,
    .set_field_bool = dynamic_set_field_bool,
    .set_field_string = dynamic_set_field_string,
    .set_field_bytes = dynamic_set_field_bytes,
    .set_field_uuid = dynamic_set_field_uuid,
    .create_list = dynamic_create_list,
    .add_list_item_int = dynamic_add_list_item_int,
    .add_list_item_int64 = dynamic_add_list_item_int64,
    .add_list_item_double = dynamic_add_list_item_double,
    .add_list_item_bool = dynamic_add_list_item_bool,
    .add_list_item_string = dynamic_add_list_item_string,
    .add_list_item_object = dynamic_add_list_item_object,
    .set_field_list = dynamic_set_field_value,
    .set_field_object = dynamic_set_field_value,
    .create_set = dynamic_create_set,
    .add_set_item_int = dynamic_add_list_item_int,
    .add_set_item_double = dynamic_add_list_item_double,
    .add_set_item_bool = dynamic_add_list_item_bool,
    .add_set_item_string = dynamic_add_list_item_string,
    .set_field_set = dynamic_set_field_value,
    .create_map = dynamic_create_map,
    .add_map_entry_string_string = dynamic_add_map_entry_string_string,
    .add_map_entry_string_int = dynamic_add_map_entry_string_int,
    .add_map_entry_string_double = dynamic_add_map_entry_string_double,
    .add_map_entry_string_bool = dynamic_add_map_entry_string_bool,
    .set_field_map = dynamic_set_field_value};

static DataBindValue *container_noop(void) { return NULL; }
static void add_i32_noop(DataBindValue *v, int32_t x) {
  (void)v;
  (void)x;
}
static void add_i64_noop(DataBindValue *v, int64_t x) {
  (void)v;
  (void)x;
}
static void add_dbl_noop(DataBindValue *v, double x) {
  (void)v;
  (void)x;
}
static void add_bool_noop(DataBindValue *v, int x) {
  (void)v;
  (void)x;
}
static void add_str_noop(DataBindValue *v, const char *s) {
  (void)v;
  (void)s;
}
static void add_obj_noop(DataBindValue *v, DataBindValue *o) {
  (void)v;
  (void)o;
}
static void set_container_noop(DataBindValue *o, const char *n, DataBindValue *v) {
  (void)o;
  (void)n;
  (void)v;
}
static void add_map_str_str_noop(DataBindValue *m, const char *k, const char *v) {
  (void)m;
  (void)k;
  (void)v;
}
static void add_map_str_int_noop(DataBindValue *m, const char *k, int32_t v) {
  (void)m;
  (void)k;
  (void)v;
}
static void add_map_str_dbl_noop(DataBindValue *m, const char *k, double v) {
  (void)m;
  (void)k;
  (void)v;
}
static void add_map_str_bool_noop(DataBindValue *m, const char *k, int v) {
  (void)m;
  (void)k;
  (void)v;
}
static DataBindValue *create_value_noop(void) { return NULL; }

static const data_bind_runtime_api_t MIR_OUTPUT_API = {
    .create_object = create_value_noop,
    .set_field_int = set_i32_noop,
    .set_field_int64 = set_i64_noop,
    .set_field_double = set_dbl_noop,
    .set_field_bool = set_bool_noop,
    .set_field_string = set_str_noop,
    .set_field_bytes = set_bytes_noop,
    .set_field_uuid = set_uuid_noop,
    .create_list = create_value_noop,
    .add_list_item_int = add_i32_noop,
    .add_list_item_int64 = add_i64_noop,
    .add_list_item_double = add_dbl_noop,
    .add_list_item_bool = add_bool_noop,
    .add_list_item_string = add_str_noop,
    .add_list_item_object = add_obj_noop,
    .set_field_list = set_container_noop,
    .set_field_object = set_container_noop,
    .create_set = create_value_noop,
    .add_set_item_int = add_i32_noop,
    .add_set_item_double = add_dbl_noop,
    .add_set_item_bool = add_bool_noop,
    .add_set_item_string = add_str_noop,
    .set_field_set = set_container_noop,
    .create_map = create_value_noop,
    .add_map_entry_string_string = add_map_str_str_noop,
    .add_map_entry_string_int = add_map_str_int_noop,
    .add_map_entry_string_double = add_map_str_dbl_noop,
    .add_map_entry_string_bool = add_map_str_bool_noop,
    .set_field_map = set_container_noop};

typedef enum data_bind_text_kind {
  DB_TEXT_NUMBER,
  DB_TEXT_INTEGER,
  DB_TEXT_STRING,
  DB_TEXT_BYTES,
  DB_TEXT_BOOL,
  DB_TEXT_UUID,
  DB_TEXT_DATETIME,
  DB_TEXT_DATE,
  DB_TEXT_TIME,
  DB_TEXT_DURATION,
  DB_TEXT_DECIMAL,
  DB_TEXT_BIGINT,
  DB_TEXT_MONEY,
  DB_TEXT_UNSUPPORTED
} data_bind_text_kind_t;

typedef struct data_bind_csv_headers {
  char **names;
  size_t count;
  size_t capacity;
} data_bind_csv_headers_t;

typedef struct data_bind_index_list {
  size_t *values;
  size_t count;
  size_t capacity;
} data_bind_index_list_t;

static int set_codec_error(DataBind *codec, const char *fmt, ...) {
  va_list ap;
  if (codec == NULL) return 0;
  va_start(ap, fmt);
  vsnprintf(codec->error, sizeof(codec->error), fmt, ap);
  va_end(ap);
  return 0;
}

/**
 * @brief Compute a simple hash of schema text for caching.
 * Uses FNV-1a hash algorithm for simplicity.
 */
static void compute_schema_hash(const char *schema_text, size_t len, char *hash_out) {
  uint64_t hash = 14695981039346656037ULL;  /* FNV offset basis */
  size_t i;
  if (schema_text == NULL || hash_out == NULL) return;
  
  for (i = 0; i < len; i++) {
    hash ^= (uint64_t)(unsigned char)schema_text[i];
    hash *= 1099511628211ULL;  /* FNV prime */
  }
  
  snprintf(hash_out, 65, "%016llx", (unsigned long long)hash);
}

static mir_cache_entry_t *mir_cache_find(const char *schema_hash) {
  mir_cache_entry_t *entry;
  if (!g_mir_cache_enabled || schema_hash == NULL) return NULL;
  
  for (entry = g_mir_cache_head; entry != NULL; entry = entry->next) {
    if (strcmp(entry->schema_hash, schema_hash) == 0) {
      return entry;
    }
  }
  return NULL;
}

static mir_cache_entry_t *mir_cache_insert(const char *schema_hash, MIR_context_t ctx,
                                            mir_func_node_t *func_head) {
  mir_cache_entry_t *entry;
  if (!g_mir_cache_enabled || schema_hash == NULL || ctx == NULL) return NULL;
  
  entry = (mir_cache_entry_t *)malloc(sizeof(*entry));
  if (entry == NULL) return NULL;
  
  snprintf(entry->schema_hash, sizeof(entry->schema_hash), "%s", schema_hash);
  entry->shared_ctx = ctx;
  entry->func_head = func_head;
  entry->ref_count = 1;
  entry->next = g_mir_cache_head;
  g_mir_cache_head = entry;
  
  return entry;
}

static void mir_cache_release(const char *schema_hash) {
  mir_cache_entry_t **prev_ptr = &g_mir_cache_head;
  mir_cache_entry_t *entry;
  
  if (!g_mir_cache_enabled || schema_hash == NULL) return;
  
  entry = g_mir_cache_head;
  while (entry != NULL) {
    if (strcmp(entry->schema_hash, schema_hash) == 0) {
      entry->ref_count--;
      if (entry->ref_count <= 0) {
        /* Remove from cache and free resources */
        *prev_ptr = entry->next;
        if (entry->shared_ctx != NULL) {
          MIR_finish(entry->shared_ctx);
        }
        /* Free func_head nodes that belong to this cache entry */
        if (entry->func_head != NULL) {
          mir_func_node_t *func_node = entry->func_head;
          while (func_node != NULL) {
            mir_func_node_t *next = func_node->next;
            free(func_node->type_name);
            free(func_node);
            func_node = next;
          }
        }
        free(entry);
      }
      return;
    }
    prev_ptr = &entry->next;
    entry = entry->next;
  }
}

static void db_error_clear(DataBindError *error) {
  if (error == NULL || error->size < offsetof(DataBindError, message)) return;
  error->code = DATA_BIND_OK;
  error->line = -1;
  error->column = -1;
  if (error->size >= offsetof(DataBindError, path) + sizeof(error->path))
    error->path[0] = '\0';
  if (error->size >= offsetof(DataBindError, message) + sizeof(error->message))
    error->message[0] = '\0';
}

/**
 * @brief Format error path for consistent error reporting across input formats.
 * @param out Output buffer for formatted path
 * @param out_size Size of output buffer
 * @param format Format identifier: "binary", "json", "csv", "xml"
 * @param location Format-specific location (e.g., "offset 123", "$.path", "row 5 col 3")
 */
static void db_error_format_path(char *out, size_t out_size, const char *format,
                                 const char *location) {
  if (out == NULL || out_size == 0) return;
  if (format == NULL || format[0] == '\0') {
    snprintf(out, out_size, "%s", location != NULL ? location : "");
  } else if (location != NULL && location[0] != '\0') {
    snprintf(out, out_size, "%s: %s", format, location);
  } else {
    snprintf(out, out_size, "%s", format);
  }
}

static DataBindStatus db_error_set(DataBindError *error, DataBindStatus code,
                                   const char *path, int line, int column,
                                   const char *fmt, ...) {
  va_list ap;
  if (error != NULL && error->size >= offsetof(DataBindError, message)) {
    error->code = code;
    error->line = line;
    error->column = column;
    if (error->size >= offsetof(DataBindError, path) + sizeof(error->path)) {
      snprintf(error->path, sizeof(error->path), "%s", path != NULL ? path : "");
    }
    if (error->size >= offsetof(DataBindError, message) + sizeof(error->message)) {
      va_start(ap, fmt);
      vsnprintf(error->message, sizeof(error->message), fmt, ap);
      va_end(ap);
    }
  }
  return code;
}

static DataBindStatus db_codec_error(DataBind *codec, DataBindError *error,
                                     DataBindStatus code, const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (codec != NULL) snprintf(codec->error, sizeof(codec->error), "%s", msg);
  return db_error_set(error, code, NULL, -1, -1, "%s", msg);
}

static void *codec_alloc(DataBind *codec, size_t size) {
  owned_alloc_node_t *node = NULL;
  void *ptr = calloc(1, size);
  if (ptr == NULL) return NULL;
  node = (owned_alloc_node_t *)malloc(sizeof(*node));
  if (node == NULL) {
    free(ptr);
    return NULL;
  }
  node->ptr = ptr;
  node->next = codec->owned_allocs;
  codec->owned_allocs = node;
  return ptr;
}

static char *codec_strdup(DataBind *codec, const char *src) {
  size_t len;
  char *dst;
  if (src == NULL) return NULL;
  len = strlen(src) + 1;
  dst = (char *)codec_alloc(codec, len);
  if (dst == NULL) return NULL;
  memcpy(dst, src, len);
  return dst;
}

static char *codec_strdup_n(DataBind *codec, const char *src, size_t len) {
  char *dst = (char *)codec_alloc(codec, len + 1);
  if (dst == NULL) return NULL;
  memcpy(dst, src, len);
  dst[len] = '\0';
  return dst;
}

static Node *find_child(Node *parent, const char *name) {
  size_t i;
  if (parent == NULL || parent->type != NODE_MAP) return NULL;
  for (i = 0; i < parent->data.map.count; i++)
    if (strcmp(parent->data.map.items[i]->name, name) == 0) return parent->data.map.items[i];
  return NULL;
}

static const char *get_string_val(Node *node) {
  return (node != NULL && node->type == NODE_STRING) ? node->data.string_val : NULL;
}
static int field_flag(Node *field_node, const char *name) {
  Node *n = find_child(field_node, name);
  return n != NULL && n->type == NODE_STRING && strcmp(n->data.string_val, "1") == 0;
}

static int record_flag(Node *node, const char *name) {
  return field_flag(node, name);
}

static const char *node_attribute_value(Node *node, const char *name) {
  Node *attrs;
  size_t i;
  if (node == NULL || name == NULL) return NULL;
  attrs = find_child(node, "attributes");
  if (attrs == NULL || attrs->type != NODE_LIST) return NULL;
  for (i = 0; i < attrs->data.list.count; i++) {
    Node *attr = attrs->data.list.items[i];
    const char *attr_name = get_string_val(find_child(attr, "name"));
    if (attr_name != NULL && strcmp(attr_name, name) == 0)
      return get_string_val(find_child(attr, "value"));
  }
  return NULL;
}

static const char *field_format(Node *field) {
  return node_attribute_value(field, "format");
}

static int db_text_is_empty(const char *text) {
  return text == NULL || text[0] == '\0';
}

static int db_parse_i64_text(const char *text, int64_t *out) {
  char *end = NULL;
  long long value;
  if (text == NULL || out == NULL) return 0;
  errno = 0;
  value = strtoll(text, &end, 10);
  if (errno != 0 || end == text || end == NULL || *end != '\0') return 0;
  *out = (int64_t)value;
  return 1;
}

static int db_parse_double_text(const char *text, double *out) {
  char *end = NULL;
  double value;
  if (text == NULL || out == NULL) return 0;
  errno = 0;
  value = strtod(text, &end);
  if (errno != 0 || end == text || end == NULL || *end != '\0') return 0;
  *out = value;
  return 1;
}

static int db_parse_bool_text(const char *text, int *out) {
  if (text == NULL || out == NULL) return 0;
  if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0 || strcmp(text, "yes") == 0) {
    *out = 1;
    return 1;
  }
  if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0 || strcmp(text, "no") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

static int db_is_alpha_num(char c) {
  return isalnum((unsigned char)c) != 0;
}

static int db_is_hex_char(char c) {
  return isxdigit((unsigned char)c) != 0;
}

static int db_validate_ipv4_n(const char *text, size_t len) {
  size_t pos = 0;
  int part;
  if (text == NULL || len == 0) return 0;
  for (part = 0; part < 4; part++) {
    int value = 0;
    int digits = 0;
    if (pos >= len || !isdigit((unsigned char)text[pos])) return 0;
    while (pos < len && isdigit((unsigned char)text[pos])) {
      value = value * 10 + (text[pos] - '0');
      digits++;
      if (digits > 3 || value > 255) return 0;
      pos++;
    }
    if (part < 3) {
      if (pos >= len || text[pos] != '.') return 0;
      pos++;
    }
  }
  return pos == len;
}

static int db_validate_ipv6_n(const char *text, size_t len) {
  size_t pos = 0;
  int groups = 0;
  int compressed = 0;
  if (text == NULL || len < 2) return 0;
  while (pos < len) {
    size_t start;
    int digits = 0;
    if (text[pos] == ':') {
      if (pos + 1 >= len || text[pos + 1] != ':' || compressed) return 0;
      compressed = 1;
      pos += 2;
      if (pos == len) break;
      continue;
    }
    start = pos;
    while (pos < len && db_is_hex_char(text[pos]) && digits < 4) {
      digits++;
      pos++;
    }
    if (pos < len && text[pos] == '.') {
      size_t ipv4_start = start;
      while (ipv4_start > 0 && text[ipv4_start - 1] != ':') ipv4_start--;
      if (!db_validate_ipv4_n(text + ipv4_start, len - ipv4_start)) return 0;
      groups += 2;
      pos = len;
      break;
    }
    if (digits == 0 || (pos < len && db_is_hex_char(text[pos]))) return 0;
    groups++;
    if (pos == len) break;
    if (text[pos] != ':') return 0;
    pos++;
    if (pos < len && text[pos] == ':') {
      if (compressed) return 0;
      compressed = 1;
      pos++;
      if (pos == len) break;
    }
    if (pos == len) return 0;
  }
  return compressed ? groups < 8 : groups == 8;
}

static int db_validate_ipaddr(const char *text) {
  size_t len;
  if (text == NULL) return 0;
  len = strlen(text);
  return db_validate_ipv4_n(text, len) || db_validate_ipv6_n(text, len);
}

static int db_parse_uint_n(const char *text, size_t len, int *out) {
  size_t i;
  int value = 0;
  if (text == NULL || len == 0 || out == NULL) return 0;
  for (i = 0; i < len; i++) {
    if (!isdigit((unsigned char)text[i])) return 0;
    value = value * 10 + (text[i] - '0');
    if (value > 1000) return 0;
  }
  *out = value;
  return 1;
}

static int db_validate_cidr(const char *text) {
  const char *slash;
  size_t addr_len;
  int prefix = 0;
  int is_v4;
  if (text == NULL) return 0;
  slash = strchr(text, '/');
  if (slash == NULL || slash == text || slash[1] == '\0') return 0;
  addr_len = (size_t)(slash - text);
  is_v4 = db_validate_ipv4_n(text, addr_len);
  if (!is_v4 && !db_validate_ipv6_n(text, addr_len)) return 0;
  if (!db_parse_uint_n(slash + 1, strlen(slash + 1), &prefix)) return 0;
  return prefix >= 0 && prefix <= (is_v4 ? 32 : 128);
}

static int db_validate_hostname_like(const char *text, int require_dot) {
  size_t len;
  size_t label_len = 0;
  int saw_dot = 0;
  char prev = '\0';
  size_t i;
  if (text == NULL) return 0;
  len = strlen(text);
  if (len == 0 || len > 253) return 0;
  for (i = 0; i < len; i++) {
    char c = text[i];
    if (c == '.') {
      if (label_len == 0 || prev == '-') return 0;
      saw_dot = 1;
      label_len = 0;
    } else if (db_is_alpha_num(c) || c == '-') {
      if (label_len == 0 && c == '-') return 0;
      label_len++;
      if (label_len > 63) return 0;
    } else {
      return 0;
    }
    prev = c;
  }
  if (label_len == 0 || prev == '-') return 0;
  return !require_dot || saw_dot;
}

static int db_validate_email(const char *text) {
  const char *at;
  size_t local_len;
  size_t i;
  if (text == NULL) return 0;
  at = strchr(text, '@');
  if (at == NULL || strchr(at + 1, '@') != NULL) return 0;
  local_len = (size_t)(at - text);
  if (local_len == 0 || local_len > 64 || at[1] == '\0') return 0;
  if (text[0] == '.' || text[local_len - 1] == '.') return 0;
  for (i = 0; i < local_len; i++) {
    char c = text[i];
    if (c == '.' && i > 0 && text[i - 1] == '.') return 0;
    if (!(db_is_alpha_num(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'))
      return 0;
  }
  return db_validate_hostname_like(at + 1, 1);
}

static int db_validate_scheme(const char *text, const char **after_colon) {
  const char *p;
  if (text == NULL || !isalpha((unsigned char)text[0])) return 0;
  p = text + 1;
  while (*p != '\0' && *p != ':') {
    if (!(db_is_alpha_num(*p) || *p == '+' || *p == '-' || *p == '.')) return 0;
    p++;
  }
  if (*p != ':') return 0;
  if (after_colon != NULL) *after_colon = p + 1;
  return 1;
}

static int db_validate_uri_text(const char *text, int require_authority) {
  const char *rest;
  const char *host_start;
  const char *host_end;
  const char *p;
  if (!db_validate_scheme(text, &rest) || rest[0] == '\0') return 0;
  for (p = rest; *p != '\0'; p++) {
    if ((unsigned char)*p <= 0x20 || (unsigned char)*p == 0x7f) return 0;
  }
  if (!require_authority) return 1;
  if (rest[0] != '/' || rest[1] != '/') return 0;
  host_start = rest + 2;
  if (*host_start == '\0') return 0;
  if (*host_start == '[') {
    host_end = strchr(host_start, ']');
    if (host_end == NULL || !db_validate_ipv6_n(host_start + 1, (size_t)(host_end - host_start - 1)))
      return 0;
    return host_end[1] == '\0' || host_end[1] == ':' || host_end[1] == '/' ||
           host_end[1] == '?' || host_end[1] == '#';
  }
  host_end = host_start;
  while (*host_end != '\0' && *host_end != ':' && *host_end != '/' &&
         *host_end != '?' && *host_end != '#')
    host_end++;
  if (host_end == host_start) return 0;
  if (db_validate_ipv4_n(host_start, (size_t)(host_end - host_start))) return 1;
  {
    char host[256];
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len >= sizeof(host)) return 0;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    return db_validate_hostname_like(host, 0);
  }
}

static int db_validate_macaddr(const char *text) {
  char sep;
  int i;
  if (text == NULL || strlen(text) != 17) return 0;
  sep = text[2];
  if (sep != ':' && sep != '-') return 0;
  for (i = 0; i < 17; i++) {
    if ((i + 1) % 3 == 0) {
      if (text[i] != sep) return 0;
    } else if (!db_is_hex_char(text[i])) {
      return 0;
    }
  }
  return 1;
}

static int db_validate_semver_ident(const char *text, size_t len, int numeric_core) {
  size_t i;
  if (len == 0) return 0;
  for (i = 0; i < len; i++) {
    if (numeric_core) {
      if (!isdigit((unsigned char)text[i])) return 0;
    } else if (!(db_is_alpha_num(text[i]) || text[i] == '-')) {
      return 0;
    }
  }
  if (numeric_core && len > 1 && text[0] == '0') return 0;
  return 1;
}

static int db_validate_semver_tail_n(const char *text, size_t len) {
  size_t part = 0;
  size_t i;
  if (text == NULL || len == 0) return 0;
  for (i = 0; i <= len; i++) {
    if (i == len || text[i] == '.') {
      if (!db_validate_semver_ident(text + part, i - part, 0)) return 0;
      part = i + 1;
    }
  }
  return 1;
}

static int db_validate_semver(const char *text) {
  const char *p = text;
  int part;
  if (text == NULL) return 0;
  for (part = 0; part < 3; part++) {
    const char *start = p;
    while (isdigit((unsigned char)*p)) p++;
    if (!db_validate_semver_ident(start, (size_t)(p - start), 1)) return 0;
    if (part < 2) {
      if (*p != '.') return 0;
      p++;
    }
  }
  if (*p == '-') {
    const char *start = ++p;
    while (*p != '\0' && *p != '+') p++;
    if (!db_validate_semver_tail_n(start, (size_t)(p - start))) return 0;
  }
  if (*p == '+') {
    if (!db_validate_semver_tail_n(p + 1, strlen(p + 1))) return 0;
  } else if (*p != '\0') {
    return 0;
  }
  return 1;
}

static int db_validate_hex_text(const char *text) {
  size_t i, len;
  if (text == NULL) return 0;
  len = strlen(text);
  if (len == 0) return 0;
  for (i = 0; i < len; i++)
    if (!db_is_hex_char(text[i])) return 0;
  return 1;
}

static int db_validate_base64_text(const char *text, int urlsafe) {
  size_t i, len;
  int padding = 0;
  if (text == NULL) return 0;
  len = strlen(text);
  if (len == 0 || (!urlsafe && len % 4 != 0) || (urlsafe && len % 4 == 1)) return 0;
  for (i = 0; i < len; i++) {
    char c = text[i];
    int ok = db_is_alpha_num(c) || (!urlsafe && (c == '+' || c == '/')) ||
             (urlsafe && (c == '-' || c == '_'));
    if (c == '=') {
      padding++;
      if (padding > 2) return 0;
    } else {
      if (padding > 0 || !ok) return 0;
    }
  }
  return 1;
}

static int db_validate_currency(const char *text) {
  return text != NULL && strlen(text) == 3 && text[0] >= 'A' && text[0] <= 'Z' &&
         text[1] >= 'A' && text[1] <= 'Z' && text[2] >= 'A' && text[2] <= 'Z';
}

static int db_validate_json_pointer(const char *text) {
  const char *p;
  if (text == NULL) return 0;
  if (text[0] == '\0') return 1;
  if (text[0] != '/') return 0;
  for (p = text; *p != '\0'; p++) {
    if ((unsigned char)*p < 0x20) return 0;
    if (*p == '~' && p[1] != '0' && p[1] != '1') return 0;
  }
  return 1;
}

static int db_validate_balanced_expr(const char *text, int require_dollar) {
  int paren = 0, bracket = 0;
  char quote = '\0';
  const char *p;
  if (text == NULL || text[0] == '\0') return 0;
  if (require_dollar && text[0] != '$') return 0;
  for (p = text; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f) return 0;
    if (quote != '\0') {
      if (*p == '\\' && p[1] != '\0') {
        p++;
      } else if (*p == quote) {
        quote = '\0';
      }
      continue;
    }
    if (*p == '\'' || *p == '"') {
      quote = *p;
    } else if (*p == '(') {
      paren++;
    } else if (*p == ')') {
      if (paren == 0) return 0;
      paren--;
    } else if (*p == '[') {
      bracket++;
    } else if (*p == ']') {
      if (bracket == 0) return 0;
      bracket--;
    }
  }
  return quote == '\0' && paren == 0 && bracket == 0;
}

static int db_validate_cron_field(const char *text, size_t len) {
  size_t i;
  if (text == NULL || len == 0) return 0;
  for (i = 0; i < len; i++) {
    char c = text[i];
    if (!(db_is_alpha_num(c) || c == '*' || c == '/' || c == '?' || c == ',' ||
          c == '-' || c == '.' || c == '#' || c == 'L' || c == 'W'))
      return 0;
  }
  return 1;
}

static int db_validate_cron(const char *text) {
  const char *p;
  int fields = 0;
  if (text == NULL) return 0;
  p = text;
  while (*p != '\0') {
    const char *start;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    if (!db_validate_cron_field(start, (size_t)(p - start))) return 0;
    fields++;
  }
  return fields == 5 || fields == 6 || fields == 7;
}

static int db_validate_color(const char *text) {
  size_t len;
  size_t i;
  if (text == NULL) return 0;
  len = strlen(text);
  if (len == 4 || len == 7 || len == 9) {
    if (text[0] != '#') return 0;
    for (i = 1; i < len; i++)
      if (!db_is_hex_char(text[i])) return 0;
    return 1;
  }
  return 0;
}

static int db_is_mime_token_char(char c) {
  return db_is_alpha_num(c) || c == '!' || c == '#' || c == '$' || c == '&' ||
         c == '^' || c == '_' || c == '.' || c == '+' || c == '-';
}

static int db_validate_mime_token(const char *text, size_t len) {
  size_t i;
  if (text == NULL || len == 0) return 0;
  for (i = 0; i < len; i++)
    if (!db_is_mime_token_char(text[i])) return 0;
  return 1;
}

static int db_validate_mime(const char *text) {
  const char *slash;
  if (text == NULL) return 0;
  slash = strchr(text, '/');
  if (slash == NULL || slash == text || slash[1] == '\0' || strchr(slash + 1, '/') != NULL)
    return 0;
  return db_validate_mime_token(text, (size_t)(slash - text)) &&
         db_validate_mime_token(slash + 1, strlen(slash + 1));
}

static int db_validate_regex(const char *text) {
  char *pattern;
  char *cursor;
  struct re_err err;
  struct fsm *fsm;
  if (text == NULL) return 0;
  pattern = dbv_strdup(text);
  if (pattern == NULL) return 0;
  cursor = pattern;
  memset(&err, 0, sizeof(err));
  fsm = re_comp(RE_PCRE, fsm_sgetc, &cursor, NULL, 0, &err);
  if (fsm == NULL) {
    free(pattern);
    return 0;
  }
  fsm_free(fsm);
  free(pattern);
  return 1;
}

static int db_validate_string_format(const char *format, const char *text) {
  if (format == NULL || format[0] == '\0') return 1;
  if (strcmp(format, "ipaddr") == 0 || strcmp(format, "ip") == 0)
    return db_validate_ipaddr(text);
  if (strcmp(format, "cidr") == 0) return db_validate_cidr(text);
  if (strcmp(format, "hostname") == 0) return db_validate_hostname_like(text, 0);
  if (strcmp(format, "domain") == 0) return db_validate_hostname_like(text, 1);
  if (strcmp(format, "email") == 0) return db_validate_email(text);
  if (strcmp(format, "url") == 0) return db_validate_uri_text(text, 1);
  if (strcmp(format, "uri") == 0) return db_validate_uri_text(text, 0);
  if (strcmp(format, "macaddr") == 0 || strcmp(format, "mac") == 0)
    return db_validate_macaddr(text);
  if (strcmp(format, "semver") == 0) return db_validate_semver(text);
  if (strcmp(format, "hex") == 0) return db_validate_hex_text(text);
  if (strcmp(format, "base64") == 0) return db_validate_base64_text(text, 0);
  if (strcmp(format, "base64url") == 0) return db_validate_base64_text(text, 1);
  if (strcmp(format, "currency") == 0) return db_validate_currency(text);
  if (strcmp(format, "json_pointer") == 0 || strcmp(format, "json-pointer") == 0)
    return db_validate_json_pointer(text);
  if (strcmp(format, "jsonpath") == 0 || strcmp(format, "json_path") == 0)
    return db_validate_balanced_expr(text, 1);
  if (strcmp(format, "xpath") == 0) return db_validate_balanced_expr(text, 0);
  if (strcmp(format, "cron") == 0) return db_validate_cron(text);
  if (strcmp(format, "color") == 0) return db_validate_color(text);
  if (strcmp(format, "mime") == 0 || strcmp(format, "mime_type") == 0)
    return db_validate_mime(text);
  if (strcmp(format, "regex") == 0) return db_validate_regex(text);
  return 1;
}

static int db_value_matches_field_format(Node *field, const DataBindValue *value) {
  const char *format = field_format(field);
  if (format == NULL || format[0] == '\0') return 1;
  if (value == NULL || value->kind != DATA_BIND_VALUE_STRING) return 0;
  return db_validate_string_format(format, value->data.string_val.ptr);
}

static int parse_positive_int(const char *text) {
  char *end = NULL;
  long value;
  if (text == NULL) return 0;
  value = strtol(text, &end, 10);
  if (end == text || value <= 0 || value > 0x7fffffffL) return 0;
  return (int)value;
}

static int parse_size_value(const char *text, size_t *out) {
  char *end = NULL;
  unsigned long long value;
  if (out != NULL) *out = 0;
  if (text == NULL || text[0] == '\0') return 0;
  value = strtoull(text, &end, 10);
  if (end == text || *end != '\0') return 0;
  if (out != NULL) *out = (size_t)value;
  return 1;
}

static Node *find_named_record(Node *schema_root, const char *list_name, const char *record_name) {
  Node *list = find_child(schema_root, list_name);
  size_t i;
  if (list == NULL || list->type != NODE_LIST || record_name == NULL) return NULL;
  for (i = 0; i < list->data.list.count; i++) {
    Node *record = list->data.list.items[i];
    const char *name = get_string_val(find_child(record, "name"));
    if (name != NULL && strcmp(name, record_name) == 0) return record;
  }
  return NULL;
}

static const type_meta_t *find_enum_meta(Node *schema_root, const char *enum_name) {
  Node *enum_node = find_named_record(schema_root, "enums", enum_name);
  const char *underlying =
      enum_node != NULL ? get_string_val(find_child(enum_node, "underlying_type")) : NULL;
  const type_meta_t *meta = find_type_meta(underlying != NULL ? underlying : "uint8");
  return meta != NULL ? meta : find_type_meta("uint8");
}

static const type_meta_t *find_scalar_meta(Node *schema_root, const char *type_name) {
  if (type_name == NULL) return NULL;
  if (find_named_record(schema_root, "enums", type_name) != NULL)
    return find_enum_meta(schema_root, type_name);
  return find_type_meta(type_name);
}

static Node *find_schema_record(Node *schema_root, const char *type_name) {
  Node *record;
  if (schema_root == NULL || type_name == NULL) return NULL;
  record = find_named_record(schema_root, "messages", type_name);
  if (record != NULL) return record;
  record = find_named_record(schema_root, "composites", type_name);
  if (record != NULL) return record;
  record = find_named_record(schema_root, "groups", type_name);
  if (record != NULL) return record;
  record = find_named_record(schema_root, "unions", type_name);
  if (record != NULL) return record;
  return find_named_record(schema_root, "enums", type_name);
}

static Node *find_data_record(Node *schema_root, const char *type_name) {
  Node *record;
  if (schema_root == NULL || type_name == NULL) return NULL;
  record = find_named_record(schema_root, "messages", type_name);
  if (record != NULL) return record;
  record = find_named_record(schema_root, "composites", type_name);
  if (record != NULL) return record;
  return find_named_record(schema_root, "groups", type_name);
}

static Node *find_union_record(Node *schema_root, const char *type_name) {
  return find_named_record(schema_root, "unions", type_name);
}

static Node *find_enum_record(Node *schema_root, const char *type_name) {
  return find_named_record(schema_root, "enums", type_name);
}

static int is_flags_type(Node *schema_root, const char *type_name) {
  Node *e = find_enum_record(schema_root, type_name);
  return e != NULL && record_flag(e, "is_flags");
}

static data_bind_text_kind_t bind_type_kind(Node *schema_root, const char *type) {
  if (type == NULL) return DB_TEXT_UNSUPPORTED;
  if (strcmp(type, "uuid") == 0) return DB_TEXT_UUID;
  if (strcmp(type, "datetime") == 0) return DB_TEXT_DATETIME;
  if (strcmp(type, "date") == 0) return DB_TEXT_DATE;
  if (strcmp(type, "time") == 0) return DB_TEXT_TIME;
  if (strcmp(type, "duration") == 0) return DB_TEXT_DURATION;
  if (strcmp(type, "decimal") == 0) return DB_TEXT_DECIMAL;
  if (strcmp(type, "bigint") == 0) return DB_TEXT_BIGINT;
  if (strcmp(type, "money") == 0) return DB_TEXT_MONEY;
  if (strcmp(type, "bytes") == 0) return DB_TEXT_BYTES;
  if (strcmp(type, "string") == 0) return DB_TEXT_STRING;
  if (strcmp(type, "bool") == 0) return DB_TEXT_BOOL;
  if (strcmp(type, "float") == 0 || strcmp(type, "double") == 0 ||
      strcmp(type, "f32") == 0 || strcmp(type, "f64") == 0)
    return DB_TEXT_NUMBER;
  if (strstr(type, "int") != NULL || strcmp(type, "uint8") == 0 ||
      strcmp(type, "uint16") == 0 || strcmp(type, "uint32") == 0 ||
      strcmp(type, "uint64") == 0 || strcmp(type, "u8") == 0 || strcmp(type, "u16") == 0 ||
      strcmp(type, "u32") == 0 || strcmp(type, "u64") == 0 || strcmp(type, "byte") == 0 ||
      find_enum_record(schema_root, type) != NULL)
    return DB_TEXT_INTEGER;
  return DB_TEXT_UNSUPPORTED;
}

static data_bind_text_kind_t bind_field_kind(Node *schema_root, Node *field) {
  const char *type = get_string_val(find_child(field, "type"));
  if (type == NULL || field_flag(field, "is_collection") || field_flag(field, "is_composite_ref") ||
      field_flag(field, "is_group_field"))
    return DB_TEXT_UNSUPPORTED;
  return bind_type_kind(schema_root, type);
}

static int bind_type_supported(Node *schema_root, const char *type_name) {
  return find_data_record(schema_root, type_name) != NULL ||
         find_union_record(schema_root, type_name) != NULL ||
         bind_type_kind(schema_root, type_name) != DB_TEXT_UNSUPPORTED;
}

static int bind_field_missing_allowed(Node *field) {
  return field_flag(field, "is_optional") || field_flag(field, "has_default");
}

static Node *fields_node_for_record(Node *record);
static Node *items_node_for_enum(Node *record);

static const char *enum_item_value(Node *schema_root, const char *type_name, const char *item_name) {
  Node *e = find_enum_record(schema_root, type_name);
  Node *items = items_node_for_enum(e);
  size_t i;
  if (items == NULL || item_name == NULL) return NULL;
  for (i = 0; i < items->data.list.count; i++) {
    Node *item = items->data.list.items[i];
    const char *name = get_string_val(find_child(item, "name"));
    if (name != NULL && strcmp(name, item_name) == 0)
      return get_string_val(find_child(item, "value"));
  }
  return NULL;
}

static int enum_text_value(Node *schema_root, const char *type_name, const char *text,
                           int64_t *out) {
  const char *value_text;
  if (db_parse_i64_text(text, out)) return 1;
  value_text = enum_item_value(schema_root, type_name, text);
  return value_text != NULL ? db_parse_i64_text(value_text, out) : 0;
}

static int flags_text_value(Node *schema_root, const char *type_name, const char *text,
                            int64_t *out) {
  const char *p = text;
  int64_t acc = 0;
  int any = 0;
  if (text == NULL || out == NULL) return 0;
  if (db_parse_i64_text(text, out)) return 1;
  while (*p != '\0') {
    char token[128];
    size_t len = 0;
    int64_t value = 0;
    while (*p == ' ' || *p == '\t' || *p == '|' || *p == ',' || *p == '+') p++;
    if (*p == '\0') break;
    while (*p != '\0' && *p != '|' && *p != ',' && *p != '+' &&
           *p != ' ' && *p != '\t' && len + 1 < sizeof(token))
      token[len++] = *p++;
    token[len] = '\0';
    if (len == 0 || !enum_text_value(schema_root, type_name, token, &value)) return 0;
    acc |= value;
    any = 1;
  }
  if (!any) return 0;
  *out = acc;
  return 1;
}

static int schema_text_integer_value(Node *schema_root, const char *type_name,
                                     data_bind_text_kind_t kind, const char *text,
                                     int64_t *out) {
  if (is_flags_type(schema_root, type_name)) return flags_text_value(schema_root, type_name, text, out);
  if (find_enum_record(schema_root, type_name) != NULL)
    return enum_text_value(schema_root, type_name, text, out);
  if (kind == DB_TEXT_INTEGER) return db_parse_i64_text(text, out);
  return 0;
}

static DataBindValue *bind_text_scalar(Node *schema_root, const char *type_name,
                                       data_bind_text_kind_t kind, const char *text) {
  int64_t i64 = 0;
  double dbl = 0.0;
  int b = 0;
  if (kind == DB_TEXT_UNSUPPORTED) return NULL;
  if (schema_text_integer_value(schema_root, type_name, kind, text, &i64)) {
    if (i64 >= INT32_MIN && i64 <= INT32_MAX) return dbv_int((int32_t)i64);
    return dbv_int64(i64);
  }
  switch (kind) {
  case DB_TEXT_STRING:
    return dbv_string(text != NULL ? text : "");
  case DB_TEXT_BYTES:
    return dbv_bytes((const uint8_t *)(text != NULL ? text : ""),
                     text != NULL ? strlen(text) : 0);
  case DB_TEXT_UUID:
    return dbv_uuid_text(text);
  case DB_TEXT_DATETIME:
    return dbv_datetime_text(text);
  case DB_TEXT_DATE:
    return dbv_date_text(text);
  case DB_TEXT_TIME:
    return dbv_time_text(text);
  case DB_TEXT_DURATION:
    return dbv_duration_text(text);
  case DB_TEXT_DECIMAL:
    return dbv_decimal_text(text);
  case DB_TEXT_BIGINT:
    return dbv_bigint_text(text);
  case DB_TEXT_MONEY:
    return dbv_money_text(text);
  case DB_TEXT_BOOL:
    if (!db_parse_bool_text(text, &b)) return NULL;
    return dbv_bool(b);
  case DB_TEXT_INTEGER:
    if (!db_parse_i64_text(text, &i64)) return NULL;
    if (i64 >= INT32_MIN && i64 <= INT32_MAX) return dbv_int((int32_t)i64);
    return dbv_int64(i64);
  case DB_TEXT_NUMBER:
    if (!db_parse_double_text(text, &dbl)) return NULL;
    return dbv_double(dbl);
  default:
    return NULL;
  }
}

static DataBindValue *bind_field_default(Node *schema_root, Node *field) {
  const char *default_text = get_string_val(find_child(field, "default_value"));
  const char *type = get_string_val(find_child(field, "type"));
  data_bind_text_kind_t kind;
  const char *enum_value;
  if (default_text == NULL || type == NULL) return NULL;
  kind = bind_field_kind(schema_root, field);
  if (kind == DB_TEXT_UNSUPPORTED) return NULL;
  if (kind == DB_TEXT_INTEGER) {
    if (strcmp(default_text, "true") == 0) return dbv_int(1);
    if (strcmp(default_text, "false") == 0) return dbv_int(0);
  }
  enum_value = enum_item_value(schema_root, type, default_text);
  if (enum_value != NULL) default_text = enum_value;
  return bind_text_scalar(schema_root, type, kind, default_text);
}

static int json_integer_value(Node *schema_root, const char *type_name, json_value_t *value,
                              int64_t *out) {
  if (value == NULL || out == NULL) return 0;
  if (turbo_json_type(value) == TURBO_JSON_NUMBER) {
    *out = (int64_t)turbo_json_number(value);
    return 1;
  }
  if (turbo_json_type(value) == TURBO_JSON_BOOL) {
    *out = turbo_json_bool(value) ? 1 : 0;
    return 1;
  }
  if (turbo_json_type(value) == TURBO_JSON_STRING) {
    if (is_flags_type(schema_root, type_name))
      return flags_text_value(schema_root, type_name, turbo_json_string(value), out);
    if (find_enum_record(schema_root, type_name) != NULL)
      return enum_text_value(schema_root, type_name, turbo_json_string(value), out);
    return db_parse_i64_text(turbo_json_string(value), out);
  }
  return 0;
}

static int json_flags_value(Node *schema_root, const char *type_name, json_value_t *value,
                            int64_t *out) {
  int64_t acc = 0;
  size_t i;
  if (json_integer_value(schema_root, type_name, value, out)) return 1;
  if (value == NULL || turbo_json_type(value) != TURBO_JSON_ARRAY || out == NULL) return 0;
  for (i = 0; i < turbo_json_array_size(value); i++) {
    int64_t item = 0;
    if (!json_flags_value(schema_root, type_name, turbo_json_array_get(value, i), &item)) return 0;
    acc |= item;
  }
  *out = acc;
  return 1;
}

static DataBindValue *bind_json_value(Node *schema_root, const char *type_name,
                                      data_bind_text_kind_t kind, json_value_t *value) {
  int64_t i64 = 0;
  char number_buf[64];
  char decimal_buf[64];
  char bigint_buf[64];
  if (value == NULL || kind == DB_TEXT_UNSUPPORTED) return NULL;
  if (is_flags_type(schema_root, type_name)) {
    if (!json_flags_value(schema_root, type_name, value, &i64)) return NULL;
    if (i64 >= INT32_MIN && i64 <= INT32_MAX) return dbv_int((int32_t)i64);
    return dbv_int64(i64);
  }
  if (kind == DB_TEXT_INTEGER && json_integer_value(schema_root, type_name, value, &i64)) {
    if (i64 >= INT32_MIN && i64 <= INT32_MAX) return dbv_int((int32_t)i64);
    return dbv_int64(i64);
  }
  switch (kind) {
  case DB_TEXT_STRING:
    if (turbo_json_type(value) == TURBO_JSON_STRING) {
      return dbv_string(turbo_json_string(value));
    }
    if (turbo_json_type(value) == TURBO_JSON_NUMBER) {
      snprintf(number_buf, sizeof(number_buf), "%.17g", turbo_json_number(value));
      return dbv_string(number_buf);
    }
    if (turbo_json_type(value) == TURBO_JSON_BOOL)
      return dbv_string(turbo_json_bool(value) ? "true" : "false");
    return NULL;
  case DB_TEXT_BYTES:
    if (turbo_json_type(value) != TURBO_JSON_STRING) return NULL;
    return dbv_bytes((const uint8_t *)turbo_json_string(value), turbo_json_string_len(value));
  case DB_TEXT_UUID:
    if (turbo_json_type(value) != TURBO_JSON_STRING) return NULL;
    return dbv_uuid_text(turbo_json_string(value));
  case DB_TEXT_DATETIME:
    if (turbo_json_type(value) != TURBO_JSON_STRING) return NULL;
    return dbv_datetime_text(turbo_json_string(value));
  case DB_TEXT_DATE:
    if (turbo_json_type(value) != TURBO_JSON_STRING) return NULL;
    return dbv_date_text(turbo_json_string(value));
  case DB_TEXT_TIME:
    if (turbo_json_type(value) != TURBO_JSON_STRING) return NULL;
    return dbv_time_text(turbo_json_string(value));
  case DB_TEXT_DURATION:
    if (turbo_json_type(value) == TURBO_JSON_NUMBER) return dbv_duration((int64_t)turbo_json_number(value));
    if (turbo_json_type(value) != TURBO_JSON_STRING) return NULL;
    return dbv_duration_text(turbo_json_string(value));
  case DB_TEXT_DECIMAL:
    if (turbo_json_type(value) == TURBO_JSON_STRING)
      return dbv_decimal_text(turbo_json_string(value));
    if (turbo_json_type(value) == TURBO_JSON_NUMBER) {
      snprintf(decimal_buf, sizeof(decimal_buf), "%.17g", turbo_json_number(value));
      return dbv_decimal_text(decimal_buf);
    }
    return NULL;
  case DB_TEXT_BIGINT:
    if (turbo_json_type(value) == TURBO_JSON_STRING)
      return dbv_bigint_text(turbo_json_string(value));
    if (turbo_json_type(value) == TURBO_JSON_NUMBER) {
      double n = turbo_json_number(value);
      if (!isfinite(n) || floor(n) != n || n < -9007199254740991.0 ||
          n > 9007199254740991.0)
        return NULL;
      snprintf(bigint_buf, sizeof(bigint_buf), "%.0f", n);
      return dbv_bigint_text(bigint_buf);
    }
    return NULL;
  case DB_TEXT_MONEY:
    if (turbo_json_type(value) == TURBO_JSON_STRING)
      return dbv_money_text(turbo_json_string(value));
    if (turbo_json_type(value) == TURBO_JSON_OBJECT) {
      json_value_t *amount_value = turbo_json_object_get(value, "amount");
      json_value_t *currency_value = turbo_json_object_get(value, "currency");
      DataBindMoney money;
      char amount_buf[64];
      if (amount_value == NULL || currency_value == NULL ||
          turbo_json_type(currency_value) != TURBO_JSON_STRING ||
          !db_validate_currency(turbo_json_string(currency_value)))
        return NULL;
      if (turbo_json_type(amount_value) == TURBO_JSON_STRING) {
        if (!db_parse_decimal_text(turbo_json_string(amount_value), &money.amount)) return NULL;
      } else if (turbo_json_type(amount_value) == TURBO_JSON_NUMBER) {
        snprintf(amount_buf, sizeof(amount_buf), "%.17g", turbo_json_number(amount_value));
        if (!db_parse_decimal_text(amount_buf, &money.amount)) return NULL;
      } else {
        return NULL;
      }
      memcpy(money.currency, turbo_json_string(currency_value), 3);
      money.currency[3] = '\0';
      return dbv_money(money);
    }
    return NULL;
  case DB_TEXT_BOOL:
    if (turbo_json_type(value) == TURBO_JSON_BOOL) return dbv_bool(turbo_json_bool(value));
    if (turbo_json_type(value) == TURBO_JSON_NUMBER) return dbv_bool(turbo_json_number(value) != 0.0);
    if (turbo_json_type(value) == TURBO_JSON_STRING)
      return bind_text_scalar(schema_root, type_name, kind, turbo_json_string(value));
    return NULL;
  case DB_TEXT_INTEGER:
    if (turbo_json_type(value) == TURBO_JSON_STRING)
      return bind_text_scalar(schema_root, type_name, kind, turbo_json_string(value));
    return NULL;
  case DB_TEXT_NUMBER:
    if (turbo_json_type(value) == TURBO_JSON_NUMBER) return dbv_double(turbo_json_number(value));
    if (turbo_json_type(value) == TURBO_JSON_BOOL) return dbv_double(turbo_json_bool(value) ? 1.0 : 0.0);
    if (turbo_json_type(value) == TURBO_JSON_STRING)
      return bind_text_scalar(schema_root, type_name, kind, turbo_json_string(value));
    return NULL;
  default:
    return NULL;
  }
}

static Node *union_variant(Node *union_node, const char *variant_name) {
  Node *fields = fields_node_for_record(union_node);
  size_t i;
  if (fields == NULL || variant_name == NULL) return NULL;
  for (i = 0; i < fields->data.list.count; i++) {
    Node *field = fields->data.list.items[i];
    const char *name = get_string_val(find_child(field, "name"));
    if (name != NULL && strcmp(name, variant_name) == 0) return field;
  }
  return NULL;
}

static DataBindValue *bind_json_typed_value(Node *schema_root, const char *type_name,
                                            json_value_t *value);

static DataBindValue *bind_json_array(Node *schema_root, Node *field, json_value_t *value,
                                      DataBindValueKind list_kind) {
  const char *inner_type = get_string_val(find_child(field, "inner_type"));
  data_bind_text_kind_t scalar_kind;
  DataBindValue *list;
  size_t expected = 0;
  size_t i;
  if (value == NULL || turbo_json_type(value) != TURBO_JSON_ARRAY || inner_type == NULL) return NULL;
  if (parse_size_value(get_string_val(find_child(field, "length_field")), &expected) &&
      turbo_json_array_size(value) != expected)
    return NULL;
  list = dbv_new(list_kind);
  if (list == NULL) return NULL;
  scalar_kind = bind_type_kind(schema_root, inner_type);
  for (i = 0; i < turbo_json_array_size(value); i++) {
    json_value_t *item = turbo_json_array_get(value, i);
    DataBindValue *bound;
    if (field_flag(field, "collection_element_is_composite") ||
        find_data_record(schema_root, inner_type) != NULL ||
        find_union_record(schema_root, inner_type) != NULL)
      bound = bind_json_typed_value(schema_root, inner_type, item);
    else
      bound = bind_json_value(schema_root, inner_type, scalar_kind, item);
    if (bound == NULL || !dbv_array_push(&list->data.array_val, bound)) {
      data_bind_value_free(bound);
      data_bind_value_free(list);
      return NULL;
    }
  }
  return list;
}

static DataBindValue *bind_json_record_array(Node *schema_root, const char *type_name,
                                             json_value_t *value) {
  DataBindValue *list;
  size_t i;
  if (value == NULL || turbo_json_type(value) != TURBO_JSON_ARRAY || type_name == NULL) return NULL;
  list = dbv_new(DATA_BIND_VALUE_LIST);
  if (list == NULL) return NULL;
  for (i = 0; i < turbo_json_array_size(value); i++) {
    DataBindValue *bound = bind_json_typed_value(schema_root, type_name, turbo_json_array_get(value, i));
    if (bound == NULL || !dbv_array_push(&list->data.array_val, bound)) {
      data_bind_value_free(bound);
      data_bind_value_free(list);
      return NULL;
    }
  }
  return list;
}

static DataBindValue *bind_json_map(Node *schema_root, Node *field, json_value_t *value) {
  const char *value_type = get_string_val(find_child(field, "value_type"));
  data_bind_text_kind_t value_kind = bind_type_kind(schema_root, value_type);
  DataBindValue *map;
  size_t i;
  if (value == NULL || turbo_json_type(value) != TURBO_JSON_OBJECT || value_type == NULL) return NULL;
  map = dbv_new(DATA_BIND_VALUE_MAP);
  if (map == NULL) return NULL;
  for (i = 0; i < turbo_json_object_size(value); i++) {
    const char *key = turbo_json_object_key(value, i);
    json_value_t *item = turbo_json_object_value(value, i);
    DataBindValue *bound;
    if (key == NULL || item == NULL) continue;
    if (find_data_record(schema_root, value_type) != NULL || find_union_record(schema_root, value_type) != NULL)
      bound = bind_json_typed_value(schema_root, value_type, item);
    else
      bound = bind_json_value(schema_root, value_type, value_kind, item);
    if (bound == NULL || !dbv_map_set(map, key, bound)) {
      data_bind_value_free(bound);
      data_bind_value_free(map);
      return NULL;
    }
  }
  return map;
}

static DataBindValue *bind_json_union(Node *schema_root, Node *union_node, json_value_t *object) {
  const char *variant_name;
  const char *variant_type;
  json_value_t *payload;
  Node *variant;
  DataBindValue *bound;
  DataBindValue *result;
  data_bind_text_kind_t scalar_kind;
  if (union_node == NULL || object == NULL || turbo_json_type(object) != TURBO_JSON_OBJECT ||
      turbo_json_object_size(object) != 1)
    return NULL;
  variant_name = turbo_json_object_key(object, 0);
  payload = turbo_json_object_value(object, 0);
  variant = union_variant(union_node, variant_name);
  variant_type = get_string_val(find_child(variant, "type"));
  if (variant == NULL || variant_type == NULL || payload == NULL) return NULL;
  if (find_data_record(schema_root, variant_type) != NULL || find_union_record(schema_root, variant_type) != NULL)
    bound = bind_json_typed_value(schema_root, variant_type, payload);
  else {
    scalar_kind = bind_type_kind(schema_root, variant_type);
    bound = bind_json_value(schema_root, variant_type, scalar_kind, payload);
  }
  if (bound == NULL) return NULL;
  result = dbv_new(DATA_BIND_VALUE_OBJECT);
  if (result == NULL || !dbv_object_set(result, variant_name, bound)) {
    data_bind_value_free(bound);
    data_bind_value_free(result);
    return NULL;
  }
  return result;
}

static DataBindValue *bind_json_object(Node *schema_root, Node *record, json_value_t *object) {
  DataBindValue *result;
  Node *fields;
  size_t i;
  if (record == NULL || object == NULL || turbo_json_type(object) != TURBO_JSON_OBJECT) return NULL;
  fields = fields_node_for_record(record);
  if (fields == NULL) return NULL;
  result = dbv_new(DATA_BIND_VALUE_OBJECT);
  if (result == NULL) return NULL;
  for (i = 0; i < fields->data.list.count; i++) {
    Node *field = fields->data.list.items[i];
    const char *name = get_string_val(find_child(field, "name"));
    const char *field_type = get_string_val(find_child(field, "type"));
    json_value_t *value;
    DataBindValue *bound = NULL;
    data_bind_text_kind_t kind;
    if (name == NULL) continue;
    value = turbo_json_object_get(object, name);
    if (value == NULL) {
      bound = bind_field_default(schema_root, field);
      if (bound != NULL && db_value_matches_field_format(field, bound)) {
        if (!dbv_object_set(result, name, bound)) {
          data_bind_value_free(bound);
          data_bind_value_free(result);
          return NULL;
        }
      } else if (bound != NULL) {
        data_bind_value_free(bound);
        data_bind_value_free(result);
        return NULL;
      } else if (!bind_field_missing_allowed(field)) {
        data_bind_value_free(result);
        return NULL;
      }
      continue;
    }
    if (field_flag(field, "is_group_field")) {
      bound = bind_json_record_array(schema_root, get_string_val(find_child(field, "group_type")), value);
    } else if (field_flag(field, "is_map")) {
      bound = bind_json_map(schema_root, field, value);
    } else if (field_flag(field, "is_collection")) {
      bound = bind_json_array(schema_root, field, value,
                              field_flag(field, "is_set") ? DATA_BIND_VALUE_SET : DATA_BIND_VALUE_LIST);
    } else if (field_flag(field, "is_composite_ref") && field_type != NULL) {
      bound = bind_json_typed_value(schema_root, field_type, value);
    } else if (field_type != NULL && find_union_record(schema_root, field_type) != NULL) {
      bound = bind_json_typed_value(schema_root, field_type, value);
    } else {
      kind = bind_field_kind(schema_root, field);
      bound = bind_json_value(schema_root, field_type, kind, value);
    }
    if (bound == NULL || !db_value_matches_field_format(field, bound) ||
        !dbv_object_set(result, name, bound)) {
      data_bind_value_free(bound);
      data_bind_value_free(result);
      return NULL;
    }
  }
  return result;
}

static DataBindValue *bind_json_typed_value(Node *schema_root, const char *type_name,
                                            json_value_t *value) {
  Node *record = find_data_record(schema_root, type_name);
  Node *union_node;
  data_bind_text_kind_t scalar_kind;
  if (record != NULL) return bind_json_object(schema_root, record, value);
  union_node = find_union_record(schema_root, type_name);
  if (union_node != NULL) return bind_json_union(schema_root, union_node, value);
  scalar_kind = bind_type_kind(schema_root, type_name);
  return bind_json_value(schema_root, type_name, scalar_kind, value);
}

static int xml_join_path(char *out, size_t out_size, const char *prefix, const char *name) {
  int written;
  if (out == NULL || out_size == 0 || name == NULL) return 0;
  if (prefix != NULL && prefix[0] != '\0')
    written = snprintf(out, out_size, "%s/%s", prefix, name);
  else
    written = snprintf(out, out_size, "/*/%s", name);
  return written > 0 && (size_t)written < out_size;
}

static int xml_attr_path(char *out, size_t out_size, const char *prefix, const char *name) {
  int written;
  if (out == NULL || out_size == 0 || name == NULL) return 0;
  if (prefix != NULL && prefix[0] != '\0')
    written = snprintf(out, out_size, "%s/@%s", prefix, name);
  else
    written = snprintf(out, out_size, "/*/@%s", name);
  return written > 0 && (size_t)written < out_size;
}

static int xml_children_path(char *out, size_t out_size, const char *prefix) {
  int written;
  if (out == NULL || out_size == 0) return 0;
  written = snprintf(out, out_size, "%s/*", prefix != NULL && prefix[0] != '\0' ? prefix : "/*");
  return written > 0 && (size_t)written < out_size;
}

static int xml_path_exists(turbo_xml_doc_t *doc, const char *path) {
  return doc != NULL && path != NULL && turbo_xml_xpath_count(doc, path) > 0;
}

static const char *xml_path_text(turbo_xml_doc_t *doc, const char *path) {
  turbo_xml_xpath_node_t *node;
  const char *text;
  if (doc == NULL || path == NULL) return NULL;
  node = turbo_xml_xpath_get(doc, path);
  if (node == NULL) return NULL;
  text = turbo_xml_xpath_node_text(node);
  if (text != NULL) return text;
  return turbo_xml_xpath_text(doc, path);
}

static int xml_field_path(turbo_xml_doc_t *doc, const char *prefix, const char *name,
                          char *out, size_t out_size) {
  char child[256], attr[256];
  if (!xml_join_path(child, sizeof(child), prefix, name)) return 0;
  if (xml_path_exists(doc, child)) {
    snprintf(out, out_size, "%s", child);
    return out[0] != '\0' && strlen(out) < out_size;
  }
  if (!xml_attr_path(attr, sizeof(attr), prefix, name)) return 0;
  if (xml_path_exists(doc, attr)) {
    snprintf(out, out_size, "%s", attr);
    return out[0] != '\0' && strlen(out) < out_size;
  }
  return 0;
}

static DataBindValue *bind_xml_typed_value(Node *schema_root, const char *type_name,
                                           turbo_xml_doc_t *doc, const char *path);

static DataBindValue *bind_xml_scalar_at_path(Node *schema_root, const char *type_name,
                                              data_bind_text_kind_t kind, turbo_xml_doc_t *doc,
                                              const char *path) {
  const char *text;
  if (doc == NULL || path == NULL || kind == DB_TEXT_UNSUPPORTED) return NULL;
  text = xml_path_text(doc, path);
  if (text == NULL) return NULL;
  return bind_text_scalar(schema_root, type_name, kind, text);
}

static DataBindValue *bind_xml_list_at_path(Node *schema_root, Node *field, turbo_xml_doc_t *doc,
                                            const char *path, DataBindValueKind list_kind) {
  const char *inner_type = get_string_val(find_child(field, "inner_type"));
  data_bind_text_kind_t scalar_kind = bind_type_kind(schema_root, inner_type);
  DataBindValue *list;
  turbo_xml_list_t nodes;
  size_t count = 0;
  int fixed_count;
  if (schema_root == NULL || field == NULL || doc == NULL || path == NULL || inner_type == NULL)
    return NULL;
  fixed_count = parse_size_value(get_string_val(find_child(field, "length_field")), &count);
  turbo_xml_xpath_query(doc, path, &nodes);
  if (fixed_count && (size_t)nodes.len != count) {
    turbo_xml_list_free(&nodes);
    return NULL;
  }
  list = dbv_new(list_kind);
  if (list == NULL) {
    turbo_xml_list_free(&nodes);
    return NULL;
  }
  for (int i = 0; i < nodes.len; i++) {
    char item_path[320];
    DataBindValue *item;
    if (snprintf(item_path, sizeof(item_path), "%s[%d]", path, i + 1) >= (int)sizeof(item_path)) {
      data_bind_value_free(list);
      turbo_xml_list_free(&nodes);
      return NULL;
    }
    if (field_flag(field, "collection_element_is_composite") ||
        find_data_record(schema_root, inner_type) != NULL ||
        find_union_record(schema_root, inner_type) != NULL)
      item = bind_xml_typed_value(schema_root, inner_type, doc, item_path);
    else
      item = bind_xml_scalar_at_path(schema_root, inner_type, scalar_kind, doc, item_path);
    if (item == NULL || !dbv_array_push(&list->data.array_val, item)) {
      data_bind_value_free(item);
      data_bind_value_free(list);
      turbo_xml_list_free(&nodes);
      return NULL;
    }
  }
  turbo_xml_list_free(&nodes);
  if (data_bind_value_count(list) == 0) {
    data_bind_value_free(list);
    return NULL;
  }
  return list;
}

static DataBindValue *bind_xml_map_at_path(Node *schema_root, Node *field, turbo_xml_doc_t *doc,
                                           const char *path) {
  const char *value_type = get_string_val(find_child(field, "value_type"));
  DataBindValue *map;
  turbo_xml_list_t nodes;
  char children[256];
  if (schema_root == NULL || field == NULL || doc == NULL || path == NULL || value_type == NULL)
    return NULL;
  if (!xml_children_path(children, sizeof(children), path)) return NULL;
  turbo_xml_xpath_query(doc, children, &nodes);
  map = dbv_new(DATA_BIND_VALUE_MAP);
  if (map == NULL) {
    turbo_xml_list_free(&nodes);
    return NULL;
  }
  turbo_xml_for(node, &nodes) {
    const char *key = turbo_xml_xpath_node_name((const turbo_xml_xpath_node_t *)node);
    DataBindValue *item;
    char item_path[320];
    if (key == NULL || dbv_map_has_key(map, key)) continue;
    if (!xml_join_path(item_path, sizeof(item_path), path, key)) {
      data_bind_value_free(map);
      turbo_xml_list_free(&nodes);
      return NULL;
    }
    if (find_data_record(schema_root, value_type) != NULL ||
        find_union_record(schema_root, value_type) != NULL)
      item = bind_xml_typed_value(schema_root, value_type, doc, item_path);
    else
      item = bind_xml_scalar_at_path(schema_root, value_type, bind_type_kind(schema_root, value_type),
                                     doc, item_path);
    if (item != NULL) {
      if (!dbv_map_set(map, key, item)) {
        data_bind_value_free(item);
        data_bind_value_free(map);
        turbo_xml_list_free(&nodes);
        return NULL;
      }
    } else if (!db_text_is_empty(turbo_xml_xpath_node_text((const turbo_xml_xpath_node_t *)node))) {
      data_bind_value_free(map);
      turbo_xml_list_free(&nodes);
      return NULL;
    }
  }
  turbo_xml_list_free(&nodes);
  if (data_bind_value_count(map) == 0) {
    data_bind_value_free(map);
    return NULL;
  }
  return map;
}

static DataBindValue *bind_xml_union_at_path(Node *schema_root, Node *union_node,
                                             turbo_xml_doc_t *doc, const char *path) {
  Node *fields = fields_node_for_record(union_node);
  DataBindValue *result;
  int matches = 0;
  size_t i;
  if (fields == NULL || doc == NULL || path == NULL) return NULL;
  result = dbv_new(DATA_BIND_VALUE_OBJECT);
  if (result == NULL) return NULL;
  for (i = 0; i < fields->data.list.count; i++) {
    Node *variant = fields->data.list.items[i];
    const char *name = get_string_val(find_child(variant, "name"));
    const char *variant_type = get_string_val(find_child(variant, "type"));
    data_bind_text_kind_t scalar_kind = bind_type_kind(schema_root, variant_type);
    char item_path[256];
    DataBindValue *item = NULL;
    if (name == NULL || variant_type == NULL) {
      data_bind_value_free(result);
      return NULL;
    }
    if (!xml_field_path(doc, path, name, item_path, sizeof(item_path))) continue;
    if (find_data_record(schema_root, variant_type) != NULL ||
        find_union_record(schema_root, variant_type) != NULL)
      item = bind_xml_typed_value(schema_root, variant_type, doc, item_path);
    else
      item = bind_xml_scalar_at_path(schema_root, variant_type, scalar_kind, doc, item_path);
    if (item == NULL || !dbv_object_set(result, name, item)) {
      data_bind_value_free(item);
      data_bind_value_free(result);
      return NULL;
    }
    matches++;
  }
  if (matches != 1) {
    data_bind_value_free(result);
    return NULL;
  }
  return result;
}

static DataBindValue *bind_xml_record_at_path(Node *schema_root, Node *record,
                                              turbo_xml_doc_t *doc, const char *path) {
  Node *fields = fields_node_for_record(record);
  DataBindValue *result;
  size_t i;
  if (fields == NULL || doc == NULL || path == NULL || !xml_path_exists(doc, path)) return NULL;
  result = dbv_new(DATA_BIND_VALUE_OBJECT);
  if (result == NULL) return NULL;
  for (i = 0; i < fields->data.list.count; i++) {
    Node *field = fields->data.list.items[i];
    const char *name = get_string_val(find_child(field, "name"));
    const char *field_type = get_string_val(find_child(field, "type"));
    char field_path[256];
    DataBindValue *bound = NULL;
    if (name == NULL) continue;
    if (field_flag(field, "is_group_field")) {
      if (xml_join_path(field_path, sizeof(field_path), path, name))
        bound = bind_xml_list_at_path(schema_root, field, doc, field_path, DATA_BIND_VALUE_LIST);
    } else if (field_flag(field, "is_map")) {
      if (xml_join_path(field_path, sizeof(field_path), path, name))
        bound = bind_xml_map_at_path(schema_root, field, doc, field_path);
    } else if (field_flag(field, "is_collection")) {
      if (xml_join_path(field_path, sizeof(field_path), path, name))
        bound = bind_xml_list_at_path(schema_root, field, doc, field_path,
                                      field_flag(field, "is_set") ? DATA_BIND_VALUE_SET
                                                                  : DATA_BIND_VALUE_LIST);
    } else if (field_flag(field, "is_composite_ref") || find_union_record(schema_root, field_type)) {
      if (xml_join_path(field_path, sizeof(field_path), path, name))
        bound = bind_xml_typed_value(schema_root, field_type, doc, field_path);
    } else {
      if (xml_field_path(doc, path, name, field_path, sizeof(field_path)))
        bound = bind_xml_scalar_at_path(schema_root, field_type,
                                        bind_field_kind(schema_root, field), doc, field_path);
    }
    if (bound == NULL) bound = bind_field_default(schema_root, field);
    if (bound == NULL) {
      if (bind_field_missing_allowed(field)) continue;
      data_bind_value_free(result);
      return NULL;
    }
    if (!db_value_matches_field_format(field, bound) || !dbv_object_set(result, name, bound)) {
      data_bind_value_free(bound);
      data_bind_value_free(result);
      return NULL;
    }
  }
  return result;
}

static DataBindValue *bind_xml_typed_value(Node *schema_root, const char *type_name,
                                           turbo_xml_doc_t *doc, const char *path) {
  Node *record = find_data_record(schema_root, type_name);
  Node *union_node = find_union_record(schema_root, type_name);
  data_bind_text_kind_t kind = bind_type_kind(schema_root, type_name);
  if (record != NULL) return bind_xml_record_at_path(schema_root, record, doc, path);
  if (union_node != NULL) return bind_xml_union_at_path(schema_root, union_node, doc, path);
  return bind_xml_scalar_at_path(schema_root, type_name, kind, doc, path);
}

static void csv_headers_free(data_bind_csv_headers_t *headers) {
  size_t i;
  if (headers == NULL) return;
  for (i = 0; i < headers->count; i++) free(headers->names[i]);
  free(headers->names);
  memset(headers, 0, sizeof(*headers));
}

static int csv_headers_push(data_bind_csv_headers_t *headers, const char *text, size_t len) {
  char **items;
  char *copy;
  size_t capacity;
  if (headers == NULL || text == NULL) return 0;
  if (headers->count == headers->capacity) {
    capacity = headers->capacity == 0 ? 8 : headers->capacity * 2;
    items = (char **)realloc(headers->names, capacity * sizeof(*items));
    if (items == NULL) return 0;
    headers->names = items;
    headers->capacity = capacity;
  }
  copy = (char *)malloc(len + 1);
  if (copy == NULL) return 0;
  memcpy(copy, text, len);
  copy[len] = '\0';
  headers->names[headers->count++] = copy;
  return 1;
}

static int csv_parse_header_names(const char *csv, size_t len, data_bind_csv_headers_t *headers) {
  size_t i = 0;
  char *cell = NULL;
  size_t cell_len = 0, cell_cap = 0;
  int in_quotes = 0;
  if (csv == NULL || headers == NULL) return 0;
  memset(headers, 0, sizeof(*headers));
  while (i < len) {
    char ch = csv[i++];
    if (in_quotes) {
      if (ch == '"') {
        if (i < len && csv[i] == '"') ch = csv[i++];
        else {
          in_quotes = 0;
          continue;
        }
      }
    } else if (ch == '"') {
      in_quotes = 1;
      continue;
    } else if (ch == ',' || ch == '\n' || ch == '\r') {
      if (!csv_headers_push(headers, cell != NULL ? cell : "", cell_len)) goto fail;
      cell_len = 0;
      if (ch == '\n' || ch == '\r') {
        free(cell);
        return headers->count > 0;
      }
      continue;
    }
    if (cell_len + 1 >= cell_cap) {
      size_t next_cap = cell_cap == 0 ? 32 : cell_cap * 2;
      char *next = (char *)realloc(cell, next_cap);
      if (next == NULL) goto fail;
      cell = next;
      cell_cap = next_cap;
    }
    cell[cell_len++] = ch;
  }
  if (!csv_headers_push(headers, cell != NULL ? cell : "", cell_len)) goto fail;
  free(cell);
  return headers->count > 0;
fail:
  free(cell);
  csv_headers_free(headers);
  return 0;
}

static void csv_sanitize_path(const char *path, char *out, size_t out_size) {
  size_t w = 0;
  int last_underscore = 0;
  size_t i;
  if (out == NULL || out_size == 0) return;
  if (path == NULL) {
    out[0] = '\0';
    return;
  }
  for (i = 0; path[i] != '\0' && w + 1 < out_size; i++) {
    char ch = path[i];
    if (ch == ']' || ch == ')') continue;
    if (ch == '.' || ch == '[' || ch == '(') {
      if (w > 0 && !last_underscore) {
        out[w++] = '_';
        last_underscore = 1;
      }
      continue;
    }
    out[w++] = ch;
    last_underscore = 0;
  }
  if (w > 0 && out[w - 1] == '_') w--;
  out[w] = '\0';
}

static int csv_find_named_column(turbo_csv_doc_t *doc, const char *name, size_t *out_col) {
  char typed_name[256];
  size_t col;
  if (doc == NULL || name == NULL || out_col == NULL) return 0;
  col = turbo_csv_find_column(doc, name);
  if (col < turbo_csv_column_count(doc)) {
    *out_col = col;
    return 1;
  }
  if (snprintf(typed_name, sizeof(typed_name), "%s_n", name) < (int)sizeof(typed_name)) {
    col = turbo_csv_find_column(doc, typed_name);
    if (col < turbo_csv_column_count(doc)) {
      *out_col = col;
      return 1;
    }
  }
  if (snprintf(typed_name, sizeof(typed_name), "%s_s", name) < (int)sizeof(typed_name)) {
    col = turbo_csv_find_column(doc, typed_name);
    if (col < turbo_csv_column_count(doc)) {
      *out_col = col;
      return 1;
    }
  }
  return 0;
}

static int csv_find_path_column(turbo_csv_doc_t *doc, const char *path, size_t *out_col) {
  char sanitized[256];
  if (csv_find_named_column(doc, path, out_col)) return 1;
  csv_sanitize_path(path, sanitized, sizeof(sanitized));
  if (sanitized[0] != '\0' && strcmp(sanitized, path) != 0)
    return csv_find_named_column(doc, sanitized, out_col);
  return 0;
}

static int csv_join_path(char *out, size_t out_size, const char *prefix, const char *name) {
  int written;
  if (out == NULL || out_size == 0 || name == NULL) return 0;
  if (prefix != NULL && prefix[0] != '\0')
    written = snprintf(out, out_size, "%s.%s", prefix, name);
  else
    written = snprintf(out, out_size, "%s", name);
  return written > 0 && (size_t)written < out_size;
}

static int csv_index_path(char *out, size_t out_size, const char *prefix, size_t index) {
  int written;
  if (out == NULL || out_size == 0 || prefix == NULL) return 0;
  written = snprintf(out, out_size, "%s[%zu]", prefix, index);
  return written > 0 && (size_t)written < out_size;
}

static int index_list_push(data_bind_index_list_t *indexes, size_t value) {
  size_t *items;
  size_t i, capacity;
  if (indexes == NULL) return 0;
  for (i = 0; i < indexes->count; i++)
    if (indexes->values[i] == value) return 1;
  if (indexes->count == indexes->capacity) {
    capacity = indexes->capacity == 0 ? 8 : indexes->capacity * 2;
    items = (size_t *)realloc(indexes->values, capacity * sizeof(*items));
    if (items == NULL) return 0;
    indexes->values = items;
    indexes->capacity = capacity;
  }
  indexes->values[indexes->count++] = value;
  return 1;
}

static int index_compare(const void *a, const void *b) {
  size_t lhs = *(const size_t *)a;
  size_t rhs = *(const size_t *)b;
  return (lhs > rhs) - (lhs < rhs);
}

static int csv_header_index(const char *header, const char *path, size_t *out_index) {
  char sanitized[256];
  const char *start = NULL;
  char *end = NULL;
  unsigned long value;
  size_t path_len;
  if (header == NULL || path == NULL || out_index == NULL) return 0;
  path_len = strlen(path);
  if (strncmp(header, path, path_len) == 0 && header[path_len] == '[') {
    start = header + path_len + 1;
  } else {
    csv_sanitize_path(path, sanitized, sizeof(sanitized));
    path_len = strlen(sanitized);
    if (path_len > 0 && strncmp(header, sanitized, path_len) == 0 && header[path_len] == '_')
      start = header + path_len + 1;
  }
  if (start == NULL || !isdigit((unsigned char)start[0])) return 0;
  errno = 0;
  value = strtoul(start, &end, 10);
  if (errno != 0 || end == NULL || end == start) return 0;
  if (*end != ']' && *end != '_' && *end != '.' && *end != '\0') return 0;
  *out_index = (size_t)value;
  return 1;
}

static int csv_collect_indexes(const data_bind_csv_headers_t *headers, const char *path,
                               data_bind_index_list_t *indexes) {
  size_t i;
  if (headers == NULL || path == NULL || indexes == NULL) return 0;
  for (i = 0; i < headers->count; i++) {
    size_t index = 0;
    if (csv_header_index(headers->names[i], path, &index) && !index_list_push(indexes, index))
      return 0;
  }
  if (indexes->count > 1) qsort(indexes->values, indexes->count, sizeof(size_t), index_compare);
  return indexes->count > 0;
}

static int csv_headers_have_path(const data_bind_csv_headers_t *headers, const char *path) {
  char sanitized[256];
  size_t path_len, sanitized_len, i;
  if (headers == NULL || path == NULL) return 0;
  path_len = strlen(path);
  csv_sanitize_path(path, sanitized, sizeof(sanitized));
  sanitized_len = strlen(sanitized);
  for (i = 0; i < headers->count; i++) {
    const char *header = headers->names[i];
    if (header == NULL) continue;
    if (strcmp(header, path) == 0) return 1;
    if (path_len > 0 && strncmp(header, path, path_len) == 0 &&
        (header[path_len] == '.' || header[path_len] == '[')) return 1;
    if (sanitized_len > 0 && strcmp(header, sanitized) == 0) return 1;
    if (sanitized_len > 0 && strncmp(header, sanitized, sanitized_len) == 0 &&
        header[sanitized_len] == '_') return 1;
  }
  return 0;
}

static int csv_header_matches_path(const char *header, const char *path) {
  char sanitized[256];
  size_t path_len, sanitized_len;
  if (header == NULL || path == NULL) return 0;
  path_len = strlen(path);
  csv_sanitize_path(path, sanitized, sizeof(sanitized));
  sanitized_len = strlen(sanitized);
  if (strcmp(header, path) == 0) return 1;
  if (path_len > 0 && strncmp(header, path, path_len) == 0 &&
      (header[path_len] == '.' || header[path_len] == '['))
    return 1;
  if (sanitized_len > 0 && strcmp(header, sanitized) == 0) return 1;
  if (sanitized_len > 0 && strncmp(header, sanitized, sanitized_len) == 0 &&
      header[sanitized_len] == '_')
    return 1;
  return 0;
}

static int csv_row_has_nonempty_path(turbo_csv_doc_t *doc, size_t row,
                                     const data_bind_csv_headers_t *headers, const char *path) {
  size_t i;
  if (doc == NULL || headers == NULL || path == NULL || row >= turbo_csv_row_count(doc)) return 0;
  for (i = 0; i < headers->count; i++) {
    const char *text;
    if (!csv_header_matches_path(headers->names[i], path)) continue;
    text = turbo_csv_get(doc, row, i);
    if (!db_text_is_empty(text)) return 1;
  }
  return 0;
}

static int csv_header_map_key(const char *header, const char *path, char *key, size_t key_size) {
  size_t path_len, len;
  const char *start = NULL;
  const char *end;
  if (header == NULL || path == NULL || key == NULL || key_size == 0) return 0;
  path_len = strlen(path);
  if (strncmp(header, path, path_len) == 0 && header[path_len] == '.')
    start = header + path_len + 1;
  else if (strncmp(header, path, path_len) == 0 && header[path_len] == '_')
    start = header + path_len + 1;
  if (start == NULL || start[0] == '\0') return 0;
  end = start;
  while (*end != '\0' && *end != '.' && *end != '[' && *end != '_') end++;
  len = (size_t)(end - start);
  if (len == 0 || len >= key_size) return 0;
  memcpy(key, start, len);
  key[len] = '\0';
  return 1;
}

static DataBindValue *bind_csv_typed_value(Node *schema_root, const char *type_name,
                                           turbo_csv_doc_t *doc, size_t row,
                                           const data_bind_csv_headers_t *headers,
                                           const char *path);

static DataBindValue *bind_csv_scalar_at_path(Node *schema_root, const char *type_name,
                                              data_bind_text_kind_t kind, turbo_csv_doc_t *doc,
                                              size_t row, const char *path) {
  size_t col = 0;
  const char *text;
  if (doc == NULL || path == NULL || kind == DB_TEXT_UNSUPPORTED) return NULL;
  if (!csv_find_path_column(doc, path, &col)) return NULL;
  text = turbo_csv_get(doc, row, col);
  if (text == NULL) return NULL;
  return bind_text_scalar(schema_root, type_name, kind, text);
}

static DataBindValue *bind_csv_scalar_value(Node *schema_root, const char *type_name,
                                            data_bind_text_kind_t kind, turbo_csv_doc_t *doc,
                                            size_t row) {
  size_t col = 0;
  const char *text;
  if (doc == NULL || row >= turbo_csv_row_count(doc) || kind == DB_TEXT_UNSUPPORTED) return NULL;
  if (!csv_find_path_column(doc, "value", &col)) col = 0;
  text = turbo_csv_get(doc, row, col);
  if (text == NULL) return NULL;
  return bind_text_scalar(schema_root, type_name, kind, text);
}

static DataBindValue *bind_csv_map_at_path(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                           size_t row, const data_bind_csv_headers_t *headers,
                                           const char *path) {
  const char *value_type = get_string_val(find_child(field, "value_type"));
  data_bind_text_kind_t value_kind = bind_type_kind(schema_root, value_type);
  DataBindValue *map;
  size_t i;
  if (value_type == NULL || headers == NULL || path == NULL) return NULL;
  map = dbv_new(DATA_BIND_VALUE_MAP);
  if (map == NULL) return NULL;
  for (i = 0; i < headers->count; i++) {
    char key[128], item_path[256];
    DataBindValue *item = NULL;
    if (!csv_header_map_key(headers->names[i], path, key, sizeof(key))) continue;
    if (dbv_map_has_key(map, key)) continue;
    if (!csv_join_path(item_path, sizeof(item_path), path, key)) {
      data_bind_value_free(map);
      return NULL;
    }
    if (find_data_record(schema_root, value_type) || find_union_record(schema_root, value_type))
      item = bind_csv_typed_value(schema_root, value_type, doc, row, headers, item_path);
    else
      item = bind_csv_scalar_at_path(schema_root, value_type, value_kind, doc, row, item_path);
    if (item != NULL) {
      if (!dbv_map_set(map, key, item)) {
        data_bind_value_free(item);
        data_bind_value_free(map);
        return NULL;
      }
    } else if (csv_row_has_nonempty_path(doc, row, headers, item_path)) {
      data_bind_value_free(map);
      return NULL;
    }
  }
  if (data_bind_value_count(map) == 0) {
    data_bind_value_free(map);
    return NULL;
  }
  return map;
}

static DataBindValue *bind_csv_list_at_path(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                            size_t row, const data_bind_csv_headers_t *headers,
                                            const char *path, DataBindValueKind list_kind) {
  const char *inner_type = get_string_val(find_child(field, "inner_type"));
  data_bind_text_kind_t scalar_kind = bind_type_kind(schema_root, inner_type);
  data_bind_index_list_t indexes = {0};
  DataBindValue *list;
  size_t count = 0, i;
  int fixed_count;
  if (inner_type == NULL || path == NULL) return NULL;
  fixed_count = parse_size_value(get_string_val(find_child(field, "length_field")), &count);
  if (fixed_count) {
    for (i = 0; i < count; i++)
      if (!index_list_push(&indexes, i)) goto fail_indexes;
  } else if (!csv_collect_indexes(headers, path, &indexes)) {
    goto fail_indexes;
  }
  list = dbv_new(list_kind);
  if (list == NULL) goto fail_indexes;
  for (i = 0; i < indexes.count; i++) {
    char item_path[256];
    DataBindValue *item = NULL;
    if (!csv_index_path(item_path, sizeof(item_path), path, indexes.values[i])) {
      data_bind_value_free(list);
      goto fail_indexes;
    }
    if (field_flag(field, "collection_element_is_composite") ||
        find_data_record(schema_root, inner_type) || find_union_record(schema_root, inner_type))
      item = bind_csv_typed_value(schema_root, inner_type, doc, row, headers, item_path);
    else
      item = bind_csv_scalar_at_path(schema_root, inner_type, scalar_kind, doc, row, item_path);
    if (item != NULL) {
      if (!dbv_array_push(&list->data.array_val, item)) {
        data_bind_value_free(item);
        data_bind_value_free(list);
        goto fail_indexes;
      }
    } else if (fixed_count || csv_row_has_nonempty_path(doc, row, headers, item_path)) {
      data_bind_value_free(list);
      goto fail_indexes;
    }
  }
  free(indexes.values);
  if (data_bind_value_count(list) == 0) {
    data_bind_value_free(list);
    return NULL;
  }
  return list;
fail_indexes:
  free(indexes.values);
  return NULL;
}

static DataBindValue *bind_csv_union_at_path(Node *schema_root, Node *union_node, turbo_csv_doc_t *doc,
                                             size_t row, const data_bind_csv_headers_t *headers,
                                             const char *path) {
  Node *fields = fields_node_for_record(union_node);
  DataBindValue *result;
  int matches = 0;
  size_t i;
  if (fields == NULL || path == NULL) return NULL;
  result = dbv_new(DATA_BIND_VALUE_OBJECT);
  if (result == NULL) return NULL;
  for (i = 0; i < fields->data.list.count; i++) {
    Node *variant = fields->data.list.items[i];
    const char *name = get_string_val(find_child(variant, "name"));
    const char *variant_type = get_string_val(find_child(variant, "type"));
    char item_path[256];
    DataBindValue *item;
    if (name == NULL || variant_type == NULL ||
        !csv_join_path(item_path, sizeof(item_path), path, name)) {
      data_bind_value_free(result);
      return NULL;
    }
    if (!csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
    item = bind_csv_typed_value(schema_root, variant_type, doc, row, headers, item_path);
    if (item == NULL || !dbv_object_set(result, name, item)) {
      data_bind_value_free(item);
      data_bind_value_free(result);
      return NULL;
    }
    matches++;
  }
  if (matches != 1) {
    data_bind_value_free(result);
    return NULL;
  }
  return result;
}

static DataBindValue *bind_csv_record_at_path(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                              size_t row, const data_bind_csv_headers_t *headers,
                                              const char *prefix) {
  Node *fields = fields_node_for_record(record);
  DataBindValue *result;
  size_t i;
  if (fields == NULL || doc == NULL || row >= turbo_csv_row_count(doc)) return NULL;
  if (prefix != NULL && prefix[0] != '\0' && !csv_headers_have_path(headers, prefix)) return NULL;
  result = dbv_new(DATA_BIND_VALUE_OBJECT);
  if (result == NULL) return NULL;
  for (i = 0; i < fields->data.list.count; i++) {
    Node *field = fields->data.list.items[i];
    const char *name = get_string_val(find_child(field, "name"));
    const char *field_type = get_string_val(find_child(field, "type"));
    char path[256];
    DataBindValue *bound = NULL;
    if (name == NULL || !csv_join_path(path, sizeof(path), prefix, name)) continue;
    if (field_flag(field, "is_group_field")) {
      bound = bind_csv_list_at_path(schema_root, field, doc, row, headers, path, DATA_BIND_VALUE_LIST);
    } else if (field_flag(field, "is_map")) {
      bound = bind_csv_map_at_path(schema_root, field, doc, row, headers, path);
    } else if (field_flag(field, "is_collection")) {
      bound = bind_csv_list_at_path(schema_root, field, doc, row, headers, path,
                                    field_flag(field, "is_set") ? DATA_BIND_VALUE_SET : DATA_BIND_VALUE_LIST);
    } else if (field_flag(field, "is_composite_ref") || find_union_record(schema_root, field_type)) {
      bound = bind_csv_typed_value(schema_root, field_type, doc, row, headers, path);
    } else {
      bound = bind_csv_scalar_at_path(schema_root, field_type, bind_field_kind(schema_root, field),
                                      doc, row, path);
    }
    if (bound == NULL) bound = bind_field_default(schema_root, field);
    if (bound == NULL) {
      if (bind_field_missing_allowed(field) && !csv_row_has_nonempty_path(doc, row, headers, path))
        continue;
      data_bind_value_free(result);
      return NULL;
    }
    if (!db_value_matches_field_format(field, bound) || !dbv_object_set(result, name, bound)) {
      data_bind_value_free(bound);
      data_bind_value_free(result);
      return NULL;
    }
  }
  return result;
}

static DataBindValue *bind_csv_typed_value(Node *schema_root, const char *type_name,
                                           turbo_csv_doc_t *doc, size_t row,
                                           const data_bind_csv_headers_t *headers,
                                           const char *path) {
  Node *record = find_data_record(schema_root, type_name);
  Node *union_node = find_union_record(schema_root, type_name);
  data_bind_text_kind_t kind = bind_type_kind(schema_root, type_name);
  if (record != NULL) return bind_csv_record_at_path(schema_root, record, doc, row, headers, path);
  if (union_node != NULL) return bind_csv_union_at_path(schema_root, union_node, doc, row, headers, path);
  if (path != NULL && path[0] != '\0') return bind_csv_scalar_at_path(schema_root, type_name, kind, doc, row, path);
  return bind_csv_scalar_value(schema_root, type_name, kind, doc, row);
}

static DataBindSchemaKind schema_record_kind(const char *list_name, Node *record) {
  if (record != NULL && field_flag(record, "is_flags")) return DATA_BIND_SCHEMA_FLAGS;
  if (list_name != NULL) {
    if (strcmp(list_name, "messages") == 0) return DATA_BIND_SCHEMA_MESSAGE;
    if (strcmp(list_name, "composites") == 0) return DATA_BIND_SCHEMA_COMPOSITE;
    if (strcmp(list_name, "groups") == 0) return DATA_BIND_SCHEMA_GROUP;
    if (strcmp(list_name, "enums") == 0) return DATA_BIND_SCHEMA_ENUM;
    if (strcmp(list_name, "unions") == 0) return DATA_BIND_SCHEMA_UNION;
  }
  if (field_flag(record, "is_message_decl")) return DATA_BIND_SCHEMA_MESSAGE;
  if (field_flag(record, "is_composite_decl")) return DATA_BIND_SCHEMA_COMPOSITE;
  if (field_flag(record, "is_group_decl")) return DATA_BIND_SCHEMA_GROUP;
  if (field_flag(record, "is_union_decl")) return DATA_BIND_SCHEMA_UNION;
  return DATA_BIND_SCHEMA_UNKNOWN;
}

static Node *fields_node_for_record(Node *record) {
  Node *fields = find_child(record, "fields");
  return fields != NULL && fields->type == NODE_LIST ? fields : NULL;
}

static Node *items_node_for_enum(Node *record) {
  Node *items = find_child(record, "items");
  return items != NULL && items->type == NODE_LIST ? items : NULL;
}

static size_t db_reflect_out_size(size_t requested, size_t full_size) {
  return requested != 0 && requested < full_size ? requested : full_size;
}

static int db_reflect_has_field(size_t out_size, size_t offset, size_t field_size) {
  return offset <= out_size && field_size <= out_size - offset;
}

static void db_reflect_clear(void *out, size_t requested, size_t full_size) {
  size_t out_size;
  if (out == NULL) return;
  out_size = db_reflect_out_size(requested, full_size);
  memset(out, 0, out_size);
  if (out_size >= sizeof(size_t)) *(size_t *)out = out_size;
}

#define DB_REFLECT_SET(type, out, out_size, field, value) \
  do { \
    if (db_reflect_has_field((out_size), offsetof(type, field), sizeof((out)->field))) \
      (out)->field = (value); \
  } while (0)

static int fill_schema_type(Node *record, const char *list_name, DataBindSchemaType *out) {
  Node *fields;
  Node *items;
  size_t out_size;
  const char *name;
  if (record == NULL || out == NULL) return 0;
  out_size = db_reflect_out_size(out->size, sizeof(*out));
  memset(out, 0, out_size);
  name = get_string_val(find_child(record, "name"));
  DB_REFLECT_SET(DataBindSchemaType, out, out_size, size, out_size);
  DB_REFLECT_SET(DataBindSchemaType, out, out_size, name, name);
  DB_REFLECT_SET(DataBindSchemaType, out, out_size, kind, schema_record_kind(list_name, record));
  DB_REFLECT_SET(DataBindSchemaType, out, out_size, underlying_type,
                 get_string_val(find_child(record, "underlying_type")));
  fields = fields_node_for_record(record);
  items = items_node_for_enum(record);
  DB_REFLECT_SET(DataBindSchemaType, out, out_size, field_count,
                 fields != NULL ? fields->data.list.count : 0);
  DB_REFLECT_SET(DataBindSchemaType, out, out_size, item_count,
                 items != NULL ? items->data.list.count : 0);
  if (db_reflect_has_field(out_size, offsetof(DataBindSchemaType, fixed_block_size),
                           sizeof(out->fixed_block_size)) &&
      db_reflect_has_field(out_size, offsetof(DataBindSchemaType, has_fixed_block_size),
                           sizeof(out->has_fixed_block_size))) {
    out->has_fixed_block_size =
        parse_size_value(get_string_val(find_child(record, "fixed_block_size")),
                         &out->fixed_block_size);
  }
  return name != NULL;
}

static const char *schema_field_kind(Node *schema_root, Node *field) {
  const char *field_type = get_string_val(find_child(field, "type"));
  if (field_flag(field, "is_group_field")) return "group";
  if (field_flag(field, "is_map")) return "map";
  if (field_flag(field, "is_set")) return "set";
  if (field_flag(field, "is_list")) return "list";
  if (field_flag(field, "is_collection")) return "array";
  if (field_flag(field, "is_enum_ref") ||
      (field_type != NULL && find_named_record(schema_root, "enums", field_type) != NULL))
    return "enum";
  if (field_type != NULL && find_named_record(schema_root, "unions", field_type) != NULL)
    return "union";
  if (field_flag(field, "is_composite_ref")) return "composite";
  if (field_flag(field, "is_string")) return "string";
  if (field_flag(field, "is_bytes")) return "bytes";
  if (field_flag(field, "is_numeric") || find_type_meta(field_type) != NULL) return "scalar";
  return field_type != NULL ? "custom" : "unknown";
}

static int fill_schema_field(Node *schema_root, Node *field, DataBindSchemaField *out) {
  size_t out_size;
  const char *name;
  if (schema_root == NULL || field == NULL || out == NULL) return 0;
  out_size = db_reflect_out_size(out->size, sizeof(*out));
  memset(out, 0, out_size);
  name = get_string_val(find_child(field, "name"));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, size, out_size);
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, name, name);
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, type,
                 get_string_val(find_child(field, "type")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, kind, schema_field_kind(schema_root, field));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, inner_type,
                 get_string_val(find_child(field, "inner_type")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, group_type,
                 get_string_val(find_child(field, "group_type")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, key_type,
                 get_string_val(find_child(field, "key_type")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, value_type,
                 get_string_val(find_child(field, "value_type")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, collection_kind,
                 get_string_val(find_child(field, "collection_kind")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, length,
                 get_string_val(find_child(field, "length_field")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, is_optional,
                 field_flag(field, "is_optional"));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, has_default,
                 field_flag(field, "has_default"));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, default_value,
                 get_string_val(find_child(field, "default_value")));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, is_collection,
                 field_flag(field, "is_collection"));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, is_composite,
                 field_flag(field, "is_composite_ref"));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, is_group,
                 field_flag(field, "is_group_field"));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, is_map, field_flag(field, "is_map"));
  if (db_reflect_has_field(out_size, offsetof(DataBindSchemaField, is_enum),
                           sizeof(out->is_enum))) {
    const char *type = get_string_val(find_child(field, "type"));
    out->is_enum = field_flag(field, "is_enum_ref") ||
                   (type != NULL && find_named_record(schema_root, "enums", type) != NULL);
  }
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, is_variable_size,
                 field_flag(field, "is_variable_size"));
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, is_fixed_size,
                 field_flag(field, "is_fixed_size"));
  if (db_reflect_has_field(out_size, offsetof(DataBindSchemaField, offset),
                           sizeof(out->offset)) &&
      db_reflect_has_field(out_size, offsetof(DataBindSchemaField, has_offset),
                           sizeof(out->has_offset))) {
    out->has_offset = parse_size_value(get_string_val(find_child(field, "offset")),
                                       &out->offset);
  }
  if (db_reflect_has_field(out_size, offsetof(DataBindSchemaField, size_bytes),
                           sizeof(out->size_bytes)) &&
      db_reflect_has_field(out_size, offsetof(DataBindSchemaField, has_size_bytes),
                           sizeof(out->has_size_bytes))) {
    out->has_size_bytes =
        parse_size_value(get_string_val(find_child(field, "size_bytes")), &out->size_bytes);
  }
  if (db_reflect_has_field(out_size, offsetof(DataBindSchemaField, field_size_bytes),
                           sizeof(out->field_size_bytes)) &&
      db_reflect_has_field(out_size, offsetof(DataBindSchemaField, has_field_size_bytes),
                           sizeof(out->has_field_size_bytes))) {
    out->has_field_size_bytes =
        parse_size_value(get_string_val(find_child(field, "field_size_bytes")),
                         &out->field_size_bytes);
  }
  DB_REFLECT_SET(DataBindSchemaField, out, out_size, format, field_format(field));
  return name != NULL;
}

static int emit_field_array_push(emit_field_array_t *fields, emit_field_t field) {
  emit_field_t *new_items;
  size_t new_capacity;
  if (fields->count == fields->capacity) {
    new_capacity = fields->capacity == 0 ? 8 : fields->capacity * 2;
    new_items = (emit_field_t *)realloc(fields->items, new_capacity * sizeof(*new_items));
    if (new_items == NULL) return 0;
    fields->items = new_items;
    fields->capacity = new_capacity;
  }
  fields->items[fields->count++] = field;
  return 1;
}

static void emit_field_array_free(emit_field_array_t *fields) {
  size_t i;
  if (fields == NULL) return;
  for (i = 0; i < fields->count; i++) {
    free(fields->items[i].name);
    emit_field_array_free(&fields->items[i].children);
  }
  free(fields->items);
  fields->items = NULL;
  fields->count = 0;
  fields->capacity = 0;
}

static int append_emit_field(emit_field_array_t *fields, const char *name, emit_kind_t kind,
                             const type_meta_t *meta, int size, int has_set_bytes,
                             size_t fixed_count, int group_dim, emit_field_array_t *children) {
  emit_field_t field;
  size_t name_len = strlen(name);
  memset(&field, 0, sizeof(field));
  field.name = (char *)malloc(name_len + 1);
  if (field.name == NULL) return 0;
  memcpy(field.name, name, name_len + 1);
  field.kind = kind;
  field.size = size;
  field.mir_type = meta != NULL ? meta->mir_type : MIR_T_UNDEF;
  field.is_float = meta != NULL ? meta->is_float : 0;
  field.is_64 = meta != NULL ? meta->is_64 : 0;
  field.has_set_bytes = (unsigned char)has_set_bytes;
  field.fixed_count = fixed_count;
  field.group_dim = group_dim;
  if (children != NULL) {
    field.children = *children;
    memset(children, 0, sizeof(*children));
  }
  if (!emit_field_array_push(fields, field)) {
    free(field.name);
    emit_field_array_free(&field.children);
    return 0;
  }
  return 1;
}

static int build_fields(emit_field_array_t *fields, Node *src_fields, Node *schema_root,
                        const char *prefix, int has_set_bytes);

/* Schema validation limits to prevent malicious schemas */
#define MAX_FIELD_NESTING_DEPTH 32
#define MAX_FIELD_OFFSET_BYTES (1024 * 1024 * 1024)  /* 1GB */
#define MAX_TOTAL_FIELDS 10000

typedef struct schema_validation_context {
  int nesting_depth;
  size_t total_fields;
  size_t max_offset;
  char visited_types[256][128];  /* Track visited types to detect cycles */
  size_t visited_count;
  char error[256];
} schema_validation_context_t;

static int schema_validation_type_visited(schema_validation_context_t *ctx, const char *type_name) {
  size_t i;
  if (type_name == NULL) return 0;
  for (i = 0; i < ctx->visited_count; i++) {
    if (strcmp(ctx->visited_types[i], type_name) == 0) return 1;
  }
  return 0;
}

static int schema_validation_mark_visited(schema_validation_context_t *ctx, const char *type_name) {
  if (type_name == NULL) return 0;
  if (ctx->visited_count >= 256) {
    snprintf(ctx->error, sizeof(ctx->error), "Too many nested types (max 256)");
    return 0;
  }
  snprintf(ctx->visited_types[ctx->visited_count], 128, "%s", type_name);
  ctx->visited_count++;
  return 1;
}

static void schema_validation_unmark_visited(schema_validation_context_t *ctx) {
  if (ctx->visited_count > 0) ctx->visited_count--;
}

static int validate_field_offset_safe(schema_validation_context_t *ctx, size_t offset, size_t size) {
  if (offset > MAX_FIELD_OFFSET_BYTES) {
    snprintf(ctx->error, sizeof(ctx->error), "Field offset %zu exceeds maximum %d bytes",
             offset, MAX_FIELD_OFFSET_BYTES);
    return 0;
  }
  if (size > 0 && offset + size > MAX_FIELD_OFFSET_BYTES) {
    snprintf(ctx->error, sizeof(ctx->error), "Field range [%zu, %zu) exceeds maximum",
             offset, offset + size);
    return 0;
  }
  if (offset + size > ctx->max_offset) {
    ctx->max_offset = offset + size;
  }
  return 1;
}

static int validate_schema_fields(schema_validation_context_t *ctx, Node *src_fields,
                                  Node *schema_root, const char *parent_type);

static int validate_composite_or_group_type(schema_validation_context_t *ctx, Node *schema_root,
                                            const char *type_name, const char *list_name) {
  Node *record;
  int saved_depth;
  int result;

  if (type_name == NULL) return 1;

  /* Check for circular reference */
  if (schema_validation_type_visited(ctx, type_name)) {
    snprintf(ctx->error, sizeof(ctx->error), "Circular type reference detected: %s", type_name);
    return 0;
  }

  record = find_named_record(schema_root, list_name, type_name);
  if (record == NULL) return 1;  /* Not found is not a validation error */

  /* Check nesting depth */
  if (ctx->nesting_depth >= MAX_FIELD_NESTING_DEPTH) {
    snprintf(ctx->error, sizeof(ctx->error), 
             "Field nesting depth exceeds maximum %d (in type %s)",
             MAX_FIELD_NESTING_DEPTH, type_name);
    return 0;
  }

  /* Mark as visited and recurse */
  if (!schema_validation_mark_visited(ctx, type_name)) return 0;

  saved_depth = ctx->nesting_depth;
  ctx->nesting_depth++;
  result = validate_schema_fields(ctx, find_child(record, "fields"), schema_root, type_name);
  ctx->nesting_depth = saved_depth;

  schema_validation_unmark_visited(ctx);
  return result;
}

static int validate_schema_fields(schema_validation_context_t *ctx, Node *src_fields,
                                  Node *schema_root, const char *parent_type) {
  size_t i;
  if (src_fields == NULL || src_fields->type != NODE_LIST) return 1;

  for (i = 0; i < src_fields->data.list.count; i++) {
    Node *field = src_fields->data.list.items[i];
    const char *field_name = get_string_val(find_child(field, "name"));
    const char *field_type = get_string_val(find_child(field, "type"));
    Node *offset_node = find_child(field, "offset");
    Node *size_node = find_child(field, "size");

    if (field_name == NULL) continue;

    /* Count total fields */
    ctx->total_fields++;
    if (ctx->total_fields > MAX_TOTAL_FIELDS) {
      snprintf(ctx->error, sizeof(ctx->error),
               "Total field count exceeds maximum %d", MAX_TOTAL_FIELDS);
      return 0;
    }

    /* Validate offset if present */
    if (offset_node != NULL && offset_node->type == NODE_STRING) {
      const char *offset_str = get_string_val(offset_node);
      const char *size_str = size_node != NULL ? get_string_val(size_node) : NULL;
      if (offset_str != NULL) {
        int offset_int = parse_positive_int(offset_str);
        int size_int = size_str != NULL ? parse_positive_int(size_str) : 0;
        if (offset_int >= 0 && size_int >= 0) {
          if (!validate_field_offset_safe(ctx, (size_t)offset_int, (size_t)size_int))
            return 0;
        }
      }
    }

    /* Validate composite references */
    if (field_flag(field, "is_composite_ref")) {
      if (!validate_composite_or_group_type(ctx, schema_root, field_type, "composites"))
        return 0;
      continue;
    }

    /* Validate group references */
    if (field_flag(field, "is_group_field")) {
      const char *group_type = get_string_val(find_child(field, "group_type"));
      if (!validate_composite_or_group_type(ctx, schema_root, group_type, "groups"))
        return 0;
      continue;
    }

    /* Validate collection inner types */
    if (field_flag(field, "is_collection")) {
      const char *inner_type = get_string_val(find_child(field, "inner_type"));
      if (inner_type != NULL) {
        if (!validate_composite_or_group_type(ctx, schema_root, inner_type, "composites"))
          return 0;
      }
    }
  }
  return 1;
}

static void build_full_field_name(const char *prefix, const char *field_name, char *out,
                                  size_t out_size) {
  if (prefix != NULL && prefix[0] != '\0') snprintf(out, out_size, "%s.%s", prefix, field_name);
  else snprintf(out, out_size, "%s", field_name);
}

static int build_composite_emit_fields(emit_field_array_t *fields, Node *field, Node *schema_root,
                                       const char *full_name, int has_set_bytes) {
  const char *field_type = get_string_val(find_child(field, "type"));
  Node *composite = find_named_record(schema_root, "composites", field_type);
  if (composite == NULL) return 1;
  return build_fields(fields, find_child(composite, "fields"), schema_root, full_name,
                      has_set_bytes);
}

static int build_group_emit_field(emit_field_array_t *fields, Node *field, Node *schema_root,
                                  const char *full_name, int has_set_bytes) {
  emit_field_array_t child_fields = {0};
  Node *group =
      find_named_record(schema_root, "groups", get_string_val(find_child(field, "group_type")));
  int entry_size =
      group != NULL ? parse_positive_int(get_string_val(find_child(group, "fixed_block_size"))) : 0;
  int group_dim = parse_positive_int(get_string_val(find_child(field, "group_dimension_size")));

  if (group == NULL || entry_size <= 0) return 1;
  if (group_dim <= 0) group_dim = 4;

  if (!build_fields(&child_fields, find_child(group, "fields"), schema_root, NULL, has_set_bytes)) {
    emit_field_array_free(&child_fields);
    return 0;
  }
  if (!append_emit_field(fields, full_name, EF_GROUP, NULL, entry_size, 0, 0, group_dim,
                         &child_fields)) {
    emit_field_array_free(&child_fields);
    return 0;
  }
  return 1;
}

static int build_map_collection_emit_field(emit_field_array_t *fields, Node *field,
                                           Node *schema_root, const char *full_name) {
  const char *key_type = get_string_val(find_child(field, "key_type"));
  const char *value_type = get_string_val(find_child(field, "value_type"));
  const type_meta_t *meta = NULL;

  if (key_type == NULL || value_type == NULL) return 1;
  if (strcmp(key_type, "string") != 0) return 1;
  if (strcmp(value_type, "string") == 0)
    return append_emit_field(fields, full_name, EF_MAP_STR_STR, NULL, 0, 0, 0, 0, NULL);
  if (strcmp(value_type, "bool") == 0)
    return append_emit_field(fields, full_name, EF_MAP_STR_BOOL, find_type_meta("bool"), 1, 0,
                             0, 0, NULL);

  meta = find_scalar_meta(schema_root, value_type);
  if (meta == NULL) return 1;
  if (meta->is_float)
    return append_emit_field(fields, full_name, EF_MAP_STR_DBL, meta, meta->size, 0, 0, 0, NULL);
  if (!meta->is_64)
    return append_emit_field(fields, full_name, EF_MAP_STR_INT, meta, meta->size, 0, 0, 0, NULL);
  return 1;
}

static int build_list_or_set_collection_emit_field(emit_field_array_t *fields, Node *field,
                                                   Node *schema_root, const char *full_name,
                                                   const char *collection_kind,
                                                   const char *inner_type, int count) {
  const type_meta_t *meta = NULL;

  if (strcmp(collection_kind, "list") != 0 && strcmp(collection_kind, "set") != 0 &&
      strcmp(collection_kind, "array") != 0)
    return 1;
  if (inner_type == NULL) return 1;
  if (field_flag(field, "is_fixed_size") && count <= 0) return 1;

  if (strcmp(collection_kind, "set") != 0) {
    Node *composite = find_named_record(schema_root, "composites", inner_type);
    if (composite != NULL) {
      emit_field_array_t child_fields = {0};
      int element_size =
          parse_positive_int(get_string_val(find_child(composite, "fixed_block_size")));
      if (element_size <= 0) return 1;
      if (!build_fields(&child_fields, find_child(composite, "fields"), schema_root, NULL, 0)) {
        emit_field_array_free(&child_fields);
        return 0;
      }
      if (!append_emit_field(fields, full_name, EF_LIST_OBJ, NULL, element_size, 0,
                             field_flag(field, "is_fixed_size") ? (size_t)count : 0, 0,
                             &child_fields)) {
        emit_field_array_free(&child_fields);
        return 0;
      }
      return 1;
    }
  }

  if (strcmp(inner_type, "string") == 0) {
    emit_kind_t kind = strcmp(collection_kind, "set") == 0 ? EF_SET_STR : EF_LIST_STR;
    return append_emit_field(fields, full_name, kind, NULL, 0, 0,
                             field_flag(field, "is_fixed_size") ? (size_t)count : 0, 0, NULL);
  }

  meta = find_scalar_meta(schema_root, inner_type);
  if (meta == NULL) return 1;
  if (strcmp(collection_kind, "set") == 0) {
    emit_kind_t kind;
    if (meta->is_float) kind = EF_SET_DBL;
    else if (strcmp(inner_type, "bool") == 0) kind = EF_SET_BOOL;
    else if (!meta->is_64) kind = EF_SET_INT;
    else return 1;
    return append_emit_field(fields, full_name, kind, meta, meta->size, 0, 0, 0, NULL);
  }

  return append_emit_field(
      fields, full_name,
      strcmp(inner_type, "bool") == 0 ? EF_LIST_BOOL
                                      : (meta->is_float ? EF_LIST_DBL
                                                        : (meta->is_64 ? EF_LIST_I64 : EF_LIST_INT)),
      meta, meta->size, 0, field_flag(field, "is_fixed_size") ? (size_t)count : 0, 0, NULL);
}

static int build_collection_emit_field(emit_field_array_t *fields, Node *field, Node *schema_root,
                                       const char *full_name) {
  const char *collection_kind = get_string_val(find_child(field, "collection_kind"));
  const char *field_type = get_string_val(find_child(field, "type"));
  const char *inner_type = get_string_val(find_child(field, "inner_type"));
  int count = parse_positive_int(get_string_val(find_child(field, "length_field")));

  if (collection_kind == NULL) collection_kind = field_type;
  if (strcmp(collection_kind, "map") == 0)
    return build_map_collection_emit_field(fields, field, schema_root, full_name);

  return build_list_or_set_collection_emit_field(fields, field, schema_root, full_name,
                                                 collection_kind, inner_type, count);
}

static int build_var_or_scalar_emit_field(emit_field_array_t *fields, Node *field,
                                          Node *schema_root, const char *full_name,
                                          const char *field_type, int has_set_bytes) {
  if (field_flag(field, "is_var_data")) {
    emit_kind_t kind = field_flag(field, "is_bytes") ? EF_VAR_BYTES : EF_STR;
    return append_emit_field(fields, full_name, kind, NULL, 0, has_set_bytes, 0, 0, NULL);
  }

  if (field_flag(field, "is_bytes")) {
    int size = parse_positive_int(get_string_val(find_child(field, "size_bytes")));
    if (size <= 0) return 1;
    return append_emit_field(fields, full_name, EF_FIX_BYTES, NULL, size, has_set_bytes, 0, 0,
                             NULL);
  }

  if (field_flag(field, "is_uuid") || strcmp(field_type, "uuid") == 0) {
    return append_emit_field(fields, full_name, EF_UUID, NULL, 16, 0, 0, 0, NULL);
  }

  if (field_flag(field, "is_enum_ref")) {
    const type_meta_t *meta = find_enum_meta(schema_root, field_type);
    if (meta == NULL) return 1;
    return append_emit_field(fields, full_name, meta->is_64 ? EF_I64 : EF_INT, meta, meta->size, 0,
                             0, 0, NULL);
  }

  {
    const type_meta_t *meta = find_type_meta(field_type);
    if (meta == NULL) return 1;
    return append_emit_field(fields, full_name,
                             strcmp(field_type, "bool") == 0
                                 ? EF_BOOL
                                 : (meta->is_float ? EF_DBL : (meta->is_64 ? EF_I64 : EF_INT)),
                             meta,
                             meta->size, 0, 0, 0, NULL);
  }
}

static int build_fields(emit_field_array_t *fields, Node *src_fields, Node *schema_root,
                        const char *prefix, int has_set_bytes) {
  size_t i;
  if (src_fields == NULL || src_fields->type != NODE_LIST) return 1;
  for (i = 0; i < src_fields->data.list.count; i++) {
    Node *field = src_fields->data.list.items[i];
    const char *field_name = get_string_val(find_child(field, "name"));
    const char *field_type = get_string_val(find_child(field, "type"));
    char full_name[512];
    if (field_name == NULL || field_type == NULL) continue;
    build_full_field_name(prefix, field_name, full_name, sizeof(full_name));

    if (field_flag(field, "is_composite_ref")) {
      if (!build_composite_emit_fields(fields, field, schema_root, full_name, has_set_bytes))
        return 0;
      continue;
    }

    if (field_flag(field, "is_group_field")) {
      if (!build_group_emit_field(fields, field, schema_root, full_name, has_set_bytes)) return 0;
      continue;
    }

    if (field_flag(field, "is_collection")) {
      if (!build_collection_emit_field(fields, field, schema_root, full_name)) return 0;
      continue;
    }

    if (!build_var_or_scalar_emit_field(fields, field, schema_root, full_name, field_type,
                                        has_set_bytes))
      return 0;
  }
  return 1;
}

static int validate_fields_api(DataBind *codec, const char *message_name,
                               const emit_field_array_t *fields) {
  size_t i;
  for (i = 0; i < fields->count; i++) {
    const emit_field_t *field = &fields->items[i];
    if ((field->kind == EF_LIST_INT &&
         (codec->api.create_list == NULL || codec->api.set_field_list == NULL ||
          codec->api.add_list_item_int == NULL)) ||
        (field->kind == EF_LIST_I64 &&
         (codec->api.create_list == NULL || codec->api.set_field_list == NULL ||
          codec->api.add_list_item_int64 == NULL)) ||
        (field->kind == EF_LIST_DBL &&
         (codec->api.create_list == NULL || codec->api.set_field_list == NULL ||
          codec->api.add_list_item_double == NULL)) ||
        (field->kind == EF_LIST_BOOL &&
         (codec->api.create_list == NULL || codec->api.set_field_list == NULL ||
          codec->api.add_list_item_bool == NULL)) ||
        (field->kind == EF_LIST_STR &&
         (codec->api.create_list == NULL || codec->api.set_field_list == NULL ||
          codec->api.add_list_item_string == NULL)) ||
        ((field->kind == EF_LIST_OBJ || field->kind == EF_GROUP) &&
         (codec->api.create_list == NULL || codec->api.set_field_list == NULL ||
          codec->api.add_list_item_object == NULL)) ||
        (field->kind == EF_SET_INT &&
         (codec->api.create_set == NULL || codec->api.set_field_set == NULL ||
          codec->api.add_set_item_int == NULL)) ||
        (field->kind == EF_SET_DBL &&
         (codec->api.create_set == NULL || codec->api.set_field_set == NULL ||
          codec->api.add_set_item_double == NULL)) ||
        (field->kind == EF_SET_BOOL &&
         (codec->api.create_set == NULL || codec->api.set_field_set == NULL ||
          codec->api.add_set_item_bool == NULL)) ||
        (field->kind == EF_SET_STR &&
         (codec->api.create_set == NULL || codec->api.set_field_set == NULL ||
          codec->api.add_set_item_string == NULL)) ||
        (field->kind == EF_MAP_STR_STR &&
         (codec->api.create_map == NULL || codec->api.set_field_map == NULL ||
          codec->api.add_map_entry_string_string == NULL)) ||
        (field->kind == EF_MAP_STR_INT &&
         (codec->api.create_map == NULL || codec->api.set_field_map == NULL ||
          codec->api.add_map_entry_string_int == NULL)) ||
        (field->kind == EF_MAP_STR_DBL &&
         (codec->api.create_map == NULL || codec->api.set_field_map == NULL ||
          codec->api.add_map_entry_string_double == NULL)) ||
        (field->kind == EF_MAP_STR_BOOL &&
         (codec->api.create_map == NULL || codec->api.set_field_map == NULL ||
          codec->api.add_map_entry_string_bool == NULL))) {
      return set_codec_error(codec, "Schema field '%s.%s' requires container API callbacks",
                             message_name, field->name);
    }
    if (field->kind == EF_UUID && codec->api.set_field_uuid == NULL) {
      return set_codec_error(codec, "Schema field '%s.%s' requires uuid API callback",
                             message_name, field->name);
    }
    if (field->children.count > 0 && !validate_fields_api(codec, message_name, &field->children))
      return 0;
  }
  return 1;
}

static char *builder_make_name(mir_builder_t *builder, const char *prefix, size_t id) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "%s_%zu", prefix, id);
  return codec_strdup(builder->codec, buffer);
}

static MIR_item_t builder_make_string_data(mir_builder_t *builder, const char *value) {
  char *item_name = builder_make_name(builder, "__db_str", builder->data_name_id++);
  char *bytes = NULL;
  size_t len;
  if (item_name == NULL) return NULL;
  len = strlen(value);
  bytes = codec_strdup_n(builder->codec, value, len);
  if (bytes == NULL) return NULL;
  return MIR_new_data(builder->ctx, item_name, MIR_T_U8, len + 1, bytes);
}

static void declare_external(mir_builder_t *builder, external_ref_t *ref, const char *name,
                             size_t nres, MIR_type_t *res_types, size_t nargs, MIR_var_t *args) {
  char proto_name[128];
  ref->import_item = MIR_new_import(builder->ctx, name);
  snprintf(proto_name, sizeof(proto_name), "p_%s", name);
  ref->proto_item = MIR_new_proto_arr(builder->ctx, codec_strdup(builder->codec, proto_name), nres,
                                      res_types, nargs, args);
}

static void init_externals(mir_builder_t *builder) {
  MIR_type_t ptr_result = MIR_T_P;
  MIR_var_t set_int_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t set_i64_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_I64, "value", 0}};
  MIR_var_t set_dbl_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "value", 0}};
  MIR_var_t set_bool_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t set_str_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t set_bytes_args[] = {
      {MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "data", 0}, {MIR_T_I64, "len", 0}};
  MIR_var_t set_uuid_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "data", 0}};
  MIR_var_t list_i32_args[] = {{MIR_T_P, "list", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t list_i64_args[] = {{MIR_T_P, "list", 0}, {MIR_T_I64, "value", 0}};
  MIR_var_t list_dbl_args[] = {{MIR_T_P, "list", 0}, {MIR_T_D, "value", 0}};
  MIR_var_t list_bool_args[] = {{MIR_T_P, "list", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t list_str_args[] = {{MIR_T_P, "list", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t list_obj_args[] = {{MIR_T_P, "list", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t set_list_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "list", 0}};
  MIR_var_t map_str_str_args[] = {{MIR_T_P, "map", 0}, {MIR_T_P, "key", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t map_str_int_args[] = {
      {MIR_T_P, "map", 0}, {MIR_T_P, "key", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t map_str_dbl_args[] = {{MIR_T_P, "map", 0}, {MIR_T_P, "key", 0}, {MIR_T_D, "value", 0}};
  MIR_var_t map_str_bool_args[] = {
      {MIR_T_P, "map", 0}, {MIR_T_P, "key", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t read_varstr_args[] = {{MIR_T_P, "buf", 0}, {MIR_T_I64, "offset", 0}, {MIR_T_I64, "remaining", 0}};
  MIR_var_t free_args[] = {{MIR_T_P, "ptr", 0}};

  declare_external(builder, &builder->ext.create_obj, "create_obj", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.set_int, "set_int", 0, NULL, 3, set_int_args);
  declare_external(builder, &builder->ext.set_i64, "set_int64", 0, NULL, 3, set_i64_args);
  declare_external(builder, &builder->ext.set_dbl, "set_dbl", 0, NULL, 3, set_dbl_args);
  declare_external(builder, &builder->ext.set_bool, "set_bool", 0, NULL, 3, set_bool_args);
  declare_external(builder, &builder->ext.set_str, "set_str", 0, NULL, 3, set_str_args);
  declare_external(builder, &builder->ext.set_bytes, "set_bytes", 0, NULL, 4, set_bytes_args);
  declare_external(builder, &builder->ext.set_uuid, "set_uuid", 0, NULL, 3, set_uuid_args);
  declare_external(builder, &builder->ext.create_list, "create_list", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.add_list_int, "add_list_int", 0, NULL, 2, list_i32_args);
  declare_external(builder, &builder->ext.add_list_i64, "add_list_int64", 0, NULL, 2,
                   list_i64_args);
  declare_external(builder, &builder->ext.add_list_dbl, "add_list_dbl", 0, NULL, 2, list_dbl_args);
  declare_external(builder, &builder->ext.add_list_bool, "add_list_bool", 0, NULL, 2,
                   list_bool_args);
  declare_external(builder, &builder->ext.add_list_str, "add_list_str", 0, NULL, 2, list_str_args);
  declare_external(builder, &builder->ext.add_list_obj, "add_list_obj", 0, NULL, 2, list_obj_args);
  declare_external(builder, &builder->ext.set_list, "set_list", 0, NULL, 3, set_list_args);
  declare_external(builder, &builder->ext.create_set, "create_set", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.add_set_int, "add_set_int", 0, NULL, 2, list_i32_args);
  declare_external(builder, &builder->ext.add_set_dbl, "add_set_dbl", 0, NULL, 2, list_dbl_args);
  declare_external(builder, &builder->ext.add_set_bool, "add_set_bool", 0, NULL, 2,
                   list_bool_args);
  declare_external(builder, &builder->ext.add_set_str, "add_set_str", 0, NULL, 2, list_str_args);
  declare_external(builder, &builder->ext.set_set, "set_set", 0, NULL, 3, set_list_args);
  declare_external(builder, &builder->ext.create_map, "create_map", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.add_map_str_str, "add_map_str_str", 0, NULL, 3,
                   map_str_str_args);
  declare_external(builder, &builder->ext.add_map_str_int, "add_map_str_int", 0, NULL, 3,
                   map_str_int_args);
  declare_external(builder, &builder->ext.add_map_str_dbl, "add_map_str_dbl", 0, NULL, 3,
                   map_str_dbl_args);
  declare_external(builder, &builder->ext.add_map_str_bool, "add_map_str_bool", 0, NULL, 3,
                   map_str_bool_args);
  declare_external(builder, &builder->ext.set_map, "set_map", 0, NULL, 3, set_list_args);
  declare_external(builder, &builder->ext.read_varstr, "read_varstr", 1, &ptr_result, 3,
                   read_varstr_args);
  declare_external(builder, &builder->ext.free_fn, "free", 0, NULL, 1, free_args);
}

static MIR_reg_t emitter_new_reg(mir_emitter_t *e, MIR_type_t type, const char *base_name) {
  char *name = builder_make_name(e->builder, base_name, e->builder->temp_name_id++);
  return MIR_new_func_reg(e->builder->ctx, e->func, type, name);
}
static MIR_op_t emitter_reg(mir_emitter_t *e, MIR_reg_t reg) {
  return MIR_new_reg_op(e->builder->ctx, reg);
}
static MIR_op_t emitter_label(mir_emitter_t *e, MIR_label_t label) {
  return MIR_new_label_op(e->builder->ctx, label);
}
static void emitter_append(mir_emitter_t *e, MIR_insn_t insn) {
  MIR_append_insn(e->builder->ctx, e->func_item, insn);
}

static void emitter_call(mir_emitter_t *e, external_ref_t *ref, MIR_op_t *args, size_t nargs) {
  MIR_op_t ops[8];
  size_t i;
  ops[0] = MIR_new_ref_op(e->builder->ctx, ref->proto_item);
  ops[1] = MIR_new_ref_op(e->builder->ctx, ref->import_item);
  for (i = 0; i < nargs; i++)
    ops[i + 2] = args[i];
  emitter_append(e, MIR_new_insn_arr(e->builder->ctx, MIR_CALL, nargs + 2, ops));
}

static void emitter_call_result(mir_emitter_t *e, external_ref_t *ref, MIR_reg_t result_reg,
                                MIR_op_t *args, size_t nargs) {
  MIR_op_t ops[8];
  size_t i;
  ops[0] = MIR_new_ref_op(e->builder->ctx, ref->proto_item);
  ops[1] = MIR_new_ref_op(e->builder->ctx, ref->import_item);
  ops[2] = emitter_reg(e, result_reg);
  for (i = 0; i < nargs; i++)
    ops[i + 3] = args[i];
  emitter_append(e, MIR_new_insn_arr(e->builder->ctx, MIR_CALL, nargs + 3, ops));
}

static MIR_op_t emitter_mem(mir_emitter_t *e, MIR_type_t type, MIR_reg_t off_reg, int disp) {
  return MIR_new_mem_op(e->builder->ctx, type, disp, e->buf_reg, off_reg, 1);
}
static void emitter_advance_const(mir_emitter_t *e, MIR_reg_t off_reg, int delta) {
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, off_reg),
                                 emitter_reg(e, off_reg), MIR_new_int_op(e->builder->ctx, delta)));
}
static void emitter_advance_reg(mir_emitter_t *e, MIR_reg_t off_reg, MIR_reg_t delta_reg) {
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, off_reg),
                                 emitter_reg(e, off_reg), emitter_reg(e, delta_reg)));
}

static void emitter_bounds_check_const(mir_emitter_t *e, MIR_reg_t off_reg, int needed,
                                       MIR_label_t fail_label) {
  MIR_reg_t end_reg = emitter_new_reg(e, MIR_T_I64, "__end");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, end_reg),
                                 emitter_reg(e, off_reg), MIR_new_int_op(e->builder->ctx, needed)));
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UBGT, emitter_label(e, fail_label),
                                 emitter_reg(e, end_reg), emitter_reg(e, e->len_reg)));
}

static void emitter_bounds_check_reg(mir_emitter_t *e, MIR_reg_t off_reg, MIR_reg_t needed_reg,
                                     MIR_label_t fail_label) {
  MIR_reg_t end_reg = emitter_new_reg(e, MIR_T_I64, "__end");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, end_reg),
                                 emitter_reg(e, off_reg), emitter_reg(e, needed_reg)));
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UBGT, emitter_label(e, fail_label),
                                 emitter_reg(e, end_reg), emitter_reg(e, e->len_reg)));
}

static MIR_reg_t emitter_load_u16(mir_emitter_t *e, MIR_reg_t off_reg, int disp) {
  MIR_reg_t reg = emitter_new_reg(e, MIR_T_I64, "__u16");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UEXT16, emitter_reg(e, reg),
                                 emitter_mem(e, MIR_T_U16, off_reg, disp)));
  return reg;
}

static MIR_reg_t emitter_load_u32(mir_emitter_t *e, MIR_reg_t off_reg, int disp) {
  MIR_reg_t reg = emitter_new_reg(e, MIR_T_I64, "__u32");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UEXT32, emitter_reg(e, reg),
                                 emitter_mem(e, MIR_T_U32, off_reg, disp)));
  return reg;
}

static MIR_reg_t emitter_ptr_from_off(mir_emitter_t *e, MIR_reg_t off_reg, int disp,
                                      const char *name) {
  MIR_reg_t reg = emitter_new_reg(e, MIR_T_I64, name);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, reg),
                                 emitter_reg(e, e->buf_reg), emitter_reg(e, off_reg)));
  if (disp != 0)
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, reg),
                                   emitter_reg(e, reg), MIR_new_int_op(e->builder->ctx, disp)));
  return reg;
}

static MIR_op_t emitter_int32_value(mir_emitter_t *e, const emit_field_t *field,
                                    MIR_reg_t off_reg) {
  MIR_reg_t reg;
  switch (field->mir_type) {
  case MIR_T_I8:
    reg = emitter_new_reg(e, MIR_T_I64, "__i8");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_EXT8, emitter_reg(e, reg),
                                   emitter_mem(e, MIR_T_I8, off_reg, 0)));
    return emitter_reg(e, reg);
  case MIR_T_U8:
    reg = emitter_new_reg(e, MIR_T_I64, "__u8");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UEXT8, emitter_reg(e, reg),
                                   emitter_mem(e, MIR_T_U8, off_reg, 0)));
    return emitter_reg(e, reg);
  case MIR_T_I16:
    reg = emitter_new_reg(e, MIR_T_I64, "__i16");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_EXT16, emitter_reg(e, reg),
                                   emitter_mem(e, MIR_T_I16, off_reg, 0)));
    return emitter_reg(e, reg);
  case MIR_T_U16:
    reg = emitter_new_reg(e, MIR_T_I64, "__u16v");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UEXT16, emitter_reg(e, reg),
                                   emitter_mem(e, MIR_T_U16, off_reg, 0)));
    return emitter_reg(e, reg);
  case MIR_T_U32:
    reg = emitter_new_reg(e, MIR_T_I64, "__u32v");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UEXT32, emitter_reg(e, reg),
                                   emitter_mem(e, MIR_T_U32, off_reg, 0)));
    return emitter_reg(e, reg);
  case MIR_T_I32:
  default:
    return emitter_mem(e, MIR_T_I32, off_reg, 0);
  }
}

static MIR_op_t emitter_bool_value(mir_emitter_t *e, MIR_reg_t off_reg) {
  MIR_reg_t reg = emitter_new_reg(e, MIR_T_I64, "__bool");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UEXT8, emitter_reg(e, reg),
                                 emitter_mem(e, MIR_T_U8, off_reg, 0)));
  return emitter_reg(e, reg);
}

static MIR_op_t emitter_float_to_double(mir_emitter_t *e, MIR_reg_t off_reg) {
  MIR_reg_t reg = emitter_new_reg(e, MIR_T_D, "__dbl");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_F2D, emitter_reg(e, reg),
                                 emitter_mem(e, MIR_T_F, off_reg, 0)));
  return emitter_reg(e, reg);
}

static void emit_fields_into_object(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                                    const emit_field_array_t *fields);

static void emit_scalar_field(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                              const emit_field_t *field, MIR_item_t field_name_item) {
  MIR_op_t args[4];
  if (field->kind == EF_INT || field->kind == EF_I64 || field->kind == EF_DBL ||
      field->kind == EF_BOOL) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    args[0] = emitter_reg(e, target_obj_reg);
    args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
    if (field->kind == EF_INT) {
      args[2] = emitter_int32_value(e, field, off_reg);
      emitter_call(e, &e->builder->ext.set_int, args, 3);
    } else if (field->kind == EF_I64) {
      args[2] = emitter_mem(e, field->mir_type, off_reg, 0);
      emitter_call(e, &e->builder->ext.set_i64, args, 3);
    } else if (field->kind == EF_BOOL) {
      args[2] = emitter_bool_value(e, off_reg);
      emitter_call(e, &e->builder->ext.set_bool, args, 3);
    } else {
      args[2] = field->mir_type == MIR_T_F ? emitter_float_to_double(e, off_reg)
                                           : emitter_mem(e, MIR_T_D, off_reg, 0);
      emitter_call(e, &e->builder->ext.set_dbl, args, 3);
    }
    emitter_advance_const(e, off_reg, field->size);
    return;
  }
  if (field->kind == EF_FIX_BYTES) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    if (field->has_set_bytes) {
      MIR_reg_t ptr_reg = emitter_ptr_from_off(e, off_reg, 0, "__bytes");
      args[0] = emitter_reg(e, target_obj_reg);
      args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
      args[2] = emitter_reg(e, ptr_reg);
      args[3] = MIR_new_int_op(e->builder->ctx, field->size);
      emitter_call(e, &e->builder->ext.set_bytes, args, 4);
    }
    emitter_advance_const(e, off_reg, field->size);
    return;
  }
  if (field->kind == EF_UUID) {
    MIR_reg_t ptr_reg;
    emitter_bounds_check_const(e, off_reg, 16, e->fail_label);
    ptr_reg = emitter_ptr_from_off(e, off_reg, 0, "__uuid");
    args[0] = emitter_reg(e, target_obj_reg);
    args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
    args[2] = emitter_reg(e, ptr_reg);
    emitter_call(e, &e->builder->ext.set_uuid, args, 3);
    emitter_advance_const(e, off_reg, 16);
    return;
  }
  if (field->kind == EF_STR || field->kind == EF_VAR_BYTES) {
    MIR_reg_t len_reg, total_reg;
    emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
    len_reg = emitter_load_u32(e, off_reg, 0);
    total_reg = emitter_new_reg(e, MIR_T_I64, "__var_total");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, total_reg),
                                   emitter_reg(e, len_reg), MIR_new_int_op(e->builder->ctx, 4)));
    emitter_bounds_check_reg(e, off_reg, total_reg, e->fail_label);
    if (field->kind == EF_STR) {
      MIR_reg_t str_reg = emitter_new_reg(e, MIR_T_I64, "__str");
      MIR_reg_t remaining_reg = emitter_new_reg(e, MIR_T_I64, "__str_rem");
      MIR_label_t skip_label = MIR_new_label(e->builder->ctx);
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_SUB, emitter_reg(e, remaining_reg),
                                     emitter_reg(e, e->len_reg), emitter_reg(e, off_reg)));
      args[0] = emitter_reg(e, e->buf_reg);
      args[1] = emitter_reg(e, off_reg);
      args[2] = emitter_reg(e, remaining_reg);
      emitter_call_result(e, &e->builder->ext.read_varstr, str_reg, args, 3);
      emitter_advance_reg(e, off_reg, total_reg);
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, skip_label),
                                     emitter_reg(e, str_reg), MIR_new_int_op(e->builder->ctx, 0)));
      args[0] = emitter_reg(e, target_obj_reg);
      args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
      args[2] = emitter_reg(e, str_reg);
      emitter_call(e, &e->builder->ext.set_str, args, 3);
      emitter_append(e, skip_label);
      args[0] = emitter_reg(e, str_reg);
      emitter_call(e, &e->builder->ext.free_fn, args, 1);
    } else {
      if (field->has_set_bytes) {
        MIR_reg_t ptr_reg = emitter_ptr_from_off(e, off_reg, 4, "__var_bytes");
        args[0] = emitter_reg(e, target_obj_reg);
        args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
        args[2] = emitter_reg(e, ptr_reg);
        args[3] = emitter_reg(e, len_reg);
        emitter_call(e, &e->builder->ext.set_bytes, args, 4);
      }
      emitter_advance_reg(e, off_reg, total_reg);
    }
  }
}

static void emit_list_field(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                            const emit_field_t *field, MIR_item_t field_name_item) {
  MIR_reg_t list_reg = emitter_new_reg(e, MIR_T_I64, "__list"),
            count_reg = emitter_new_reg(e, MIR_T_I64, "__count"),
            index_reg = emitter_new_reg(e, MIR_T_I64, "__idx");
  MIR_label_t loop_label = MIR_new_label(e->builder->ctx),
              done_label = MIR_new_label(e->builder->ctx);
  MIR_op_t args[4];
  emitter_call_result(e, &e->builder->ext.create_list, list_reg, NULL, 0);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, e->fail_label),
                                 emitter_reg(e, list_reg), MIR_new_int_op(e->builder->ctx, 0)));
  if (field->fixed_count > 0) {
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, count_reg),
                                   MIR_new_int_op(e->builder->ctx, field->fixed_count)));
  } else {
    emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, count_reg),
                                   emitter_reg(e, emitter_load_u32(e, off_reg, 0))));
    emitter_advance_const(e, off_reg, 4);
  }
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, index_reg),
                                 MIR_new_int_op(e->builder->ctx, 0)));
  emitter_append(e, loop_label);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UBGE, emitter_label(e, done_label),
                                 emitter_reg(e, index_reg), emitter_reg(e, count_reg)));
  if (field->kind == EF_LIST_INT || field->kind == EF_LIST_I64 || field->kind == EF_LIST_DBL ||
      field->kind == EF_LIST_BOOL) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    args[0] = emitter_reg(e, list_reg);
    if (field->kind == EF_LIST_INT) {
      args[1] = emitter_int32_value(e, field, off_reg);
      emitter_call(e, &e->builder->ext.add_list_int, args, 2);
    } else if (field->kind == EF_LIST_I64) {
      args[1] = emitter_mem(e, field->mir_type, off_reg, 0);
      emitter_call(e, &e->builder->ext.add_list_i64, args, 2);
    } else if (field->kind == EF_LIST_BOOL) {
      args[1] = emitter_bool_value(e, off_reg);
      emitter_call(e, &e->builder->ext.add_list_bool, args, 2);
    } else {
      args[1] = field->mir_type == MIR_T_F ? emitter_float_to_double(e, off_reg)
                                           : emitter_mem(e, MIR_T_D, off_reg, 0);
      emitter_call(e, &e->builder->ext.add_list_dbl, args, 2);
    }
    emitter_advance_const(e, off_reg, field->size);
  } else if (field->kind == EF_LIST_STR) {
    MIR_reg_t len_reg, total_reg, str_reg;
    MIR_label_t skip_label = MIR_new_label(e->builder->ctx);
    emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
    len_reg = emitter_load_u32(e, off_reg, 0);
    total_reg = emitter_new_reg(e, MIR_T_I64, "__list_str_total");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, total_reg),
                                   emitter_reg(e, len_reg), MIR_new_int_op(e->builder->ctx, 4)));
    emitter_bounds_check_reg(e, off_reg, total_reg, e->fail_label);
    str_reg = emitter_new_reg(e, MIR_T_I64, "__list_str");
    {
      MIR_reg_t lrem_reg = emitter_new_reg(e, MIR_T_I64, "__list_str_rem");
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_SUB, emitter_reg(e, lrem_reg),
                                     emitter_reg(e, e->len_reg), emitter_reg(e, off_reg)));
      args[0] = emitter_reg(e, e->buf_reg);
      args[1] = emitter_reg(e, off_reg);
      args[2] = emitter_reg(e, lrem_reg);
      emitter_call_result(e, &e->builder->ext.read_varstr, str_reg, args, 3);
    }
    emitter_advance_reg(e, off_reg, total_reg);
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, skip_label),
                                   emitter_reg(e, str_reg), MIR_new_int_op(e->builder->ctx, 0)));
    args[0] = emitter_reg(e, list_reg);
    args[1] = emitter_reg(e, str_reg);
    emitter_call(e, &e->builder->ext.add_list_str, args, 2);
    emitter_append(e, skip_label);
    args[0] = emitter_reg(e, str_reg);
    emitter_call(e, &e->builder->ext.free_fn, args, 1);
  } else if (field->kind == EF_LIST_OBJ) {
    MIR_reg_t child_reg = emitter_new_reg(e, MIR_T_I64, "__child"),
              child_off_reg = emitter_new_reg(e, MIR_T_I64, "__child_off");
    emitter_call_result(e, &e->builder->ext.create_obj, child_reg, NULL, 0);
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, e->fail_label),
                                   emitter_reg(e, child_reg), MIR_new_int_op(e->builder->ctx, 0)));
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, child_off_reg),
                                   emitter_reg(e, off_reg)));
    emit_fields_into_object(e, child_reg, child_off_reg, &field->children);
    args[0] = emitter_reg(e, list_reg);
    args[1] = emitter_reg(e, child_reg);
    emitter_call(e, &e->builder->ext.add_list_obj, args, 2);
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, off_reg),
                                   emitter_reg(e, child_off_reg)));
  }
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, index_reg),
                                 emitter_reg(e, index_reg), MIR_new_int_op(e->builder->ctx, 1)));
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_JMP, emitter_label(e, loop_label)));
  emitter_append(e, done_label);
  args[0] = emitter_reg(e, target_obj_reg);
  args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
  args[2] = emitter_reg(e, list_reg);
  emitter_call(e, &e->builder->ext.set_list, args, 3);
}

static void emit_set_field(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                           const emit_field_t *field, MIR_item_t field_name_item) {
  MIR_reg_t set_reg = emitter_new_reg(e, MIR_T_I64, "__set"),
            count_reg = emitter_new_reg(e, MIR_T_I64, "__count"),
            index_reg = emitter_new_reg(e, MIR_T_I64, "__idx");
  MIR_label_t loop_label = MIR_new_label(e->builder->ctx),
              done_label = MIR_new_label(e->builder->ctx);
  MIR_op_t args[4];
  emitter_call_result(e, &e->builder->ext.create_set, set_reg, NULL, 0);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, e->fail_label),
                                 emitter_reg(e, set_reg), MIR_new_int_op(e->builder->ctx, 0)));
  emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, count_reg),
                                 emitter_reg(e, emitter_load_u32(e, off_reg, 0))));
  emitter_advance_const(e, off_reg, 4);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, index_reg),
                                 MIR_new_int_op(e->builder->ctx, 0)));
  emitter_append(e, loop_label);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UBGE, emitter_label(e, done_label),
                                 emitter_reg(e, index_reg), emitter_reg(e, count_reg)));
  args[0] = emitter_reg(e, set_reg);
  if (field->kind == EF_SET_INT) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    args[1] = emitter_int32_value(e, field, off_reg);
    emitter_call(e, &e->builder->ext.add_set_int, args, 2);
    emitter_advance_const(e, off_reg, field->size);
  } else if (field->kind == EF_SET_DBL) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    args[1] = field->mir_type == MIR_T_F ? emitter_float_to_double(e, off_reg)
                                         : emitter_mem(e, MIR_T_D, off_reg, 0);
    emitter_call(e, &e->builder->ext.add_set_dbl, args, 2);
    emitter_advance_const(e, off_reg, field->size);
  } else if (field->kind == EF_SET_BOOL) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    args[1] = emitter_bool_value(e, off_reg);
    emitter_call(e, &e->builder->ext.add_set_bool, args, 2);
    emitter_advance_const(e, off_reg, field->size);
  } else if (field->kind == EF_SET_STR) {
    MIR_reg_t len_reg, total_reg, str_reg;
    MIR_label_t skip_label = MIR_new_label(e->builder->ctx);
    emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
    len_reg = emitter_load_u32(e, off_reg, 0);
    total_reg = emitter_new_reg(e, MIR_T_I64, "__set_str_total");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, total_reg),
                                   emitter_reg(e, len_reg), MIR_new_int_op(e->builder->ctx, 4)));
    emitter_bounds_check_reg(e, off_reg, total_reg, e->fail_label);
    str_reg = emitter_new_reg(e, MIR_T_I64, "__set_str");
    {
      MIR_reg_t srem_reg = emitter_new_reg(e, MIR_T_I64, "__set_str_rem");
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_SUB, emitter_reg(e, srem_reg),
                                     emitter_reg(e, e->len_reg), emitter_reg(e, off_reg)));
      args[0] = emitter_reg(e, e->buf_reg);
      args[1] = emitter_reg(e, off_reg);
      args[2] = emitter_reg(e, srem_reg);
      emitter_call_result(e, &e->builder->ext.read_varstr, str_reg, args, 3);
    }
    emitter_advance_reg(e, off_reg, total_reg);
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, skip_label),
                                   emitter_reg(e, str_reg), MIR_new_int_op(e->builder->ctx, 0)));
    args[0] = emitter_reg(e, set_reg);
    args[1] = emitter_reg(e, str_reg);
    emitter_call(e, &e->builder->ext.add_set_str, args, 2);
    emitter_append(e, skip_label);
    args[0] = emitter_reg(e, str_reg);
    emitter_call(e, &e->builder->ext.free_fn, args, 1);
  }
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, index_reg),
                                 emitter_reg(e, index_reg), MIR_new_int_op(e->builder->ctx, 1)));
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_JMP, emitter_label(e, loop_label)));
  emitter_append(e, done_label);
  args[0] = emitter_reg(e, target_obj_reg);
  args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
  args[2] = emitter_reg(e, set_reg);
  emitter_call(e, &e->builder->ext.set_set, args, 3);
}

static void emit_map_field(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                           const emit_field_t *field, MIR_item_t field_name_item) {
  MIR_reg_t map_reg = emitter_new_reg(e, MIR_T_I64, "__map"),
            count_reg = emitter_new_reg(e, MIR_T_I64, "__count"),
            index_reg = emitter_new_reg(e, MIR_T_I64, "__idx");
  MIR_label_t loop_label = MIR_new_label(e->builder->ctx),
              done_label = MIR_new_label(e->builder->ctx);
  MIR_op_t args[4];
  emitter_call_result(e, &e->builder->ext.create_map, map_reg, NULL, 0);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, e->fail_label),
                                 emitter_reg(e, map_reg), MIR_new_int_op(e->builder->ctx, 0)));
  emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, count_reg),
                                 emitter_reg(e, emitter_load_u32(e, off_reg, 0))));
  emitter_advance_const(e, off_reg, 4);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, index_reg),
                                 MIR_new_int_op(e->builder->ctx, 0)));
  emitter_append(e, loop_label);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UBGE, emitter_label(e, done_label),
                                 emitter_reg(e, index_reg), emitter_reg(e, count_reg)));
  {
    MIR_reg_t key_len_reg, key_total_reg, key_reg;
    MIR_label_t skip_label = MIR_new_label(e->builder->ctx);
    emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
    key_len_reg = emitter_load_u32(e, off_reg, 0);
    key_total_reg = emitter_new_reg(e, MIR_T_I64, "__key_total");
    emitter_append(e,
                   MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, key_total_reg),
                                emitter_reg(e, key_len_reg), MIR_new_int_op(e->builder->ctx, 4)));
    emitter_bounds_check_reg(e, off_reg, key_total_reg, e->fail_label);
    key_reg = emitter_new_reg(e, MIR_T_I64, "__key");
    {
      MIR_reg_t krem_reg = emitter_new_reg(e, MIR_T_I64, "__key_rem");
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_SUB, emitter_reg(e, krem_reg),
                                     emitter_reg(e, e->len_reg), emitter_reg(e, off_reg)));
      args[0] = emitter_reg(e, e->buf_reg);
      args[1] = emitter_reg(e, off_reg);
      args[2] = emitter_reg(e, krem_reg);
      emitter_call_result(e, &e->builder->ext.read_varstr, key_reg, args, 3);
    }
    emitter_advance_reg(e, off_reg, key_total_reg);
    if (field->kind == EF_MAP_STR_STR) {
      MIR_reg_t val_len_reg, val_total_reg, val_reg;
      emitter_bounds_check_const(e, off_reg, 4, e->fail_label);
      val_len_reg = emitter_load_u32(e, off_reg, 0);
      val_total_reg = emitter_new_reg(e, MIR_T_I64, "__val_total");
      emitter_append(e,
                     MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, val_total_reg),
                                  emitter_reg(e, val_len_reg), MIR_new_int_op(e->builder->ctx, 4)));
      emitter_bounds_check_reg(e, off_reg, val_total_reg, e->fail_label);
      val_reg = emitter_new_reg(e, MIR_T_I64, "__val");
      {
        MIR_reg_t vrem_reg = emitter_new_reg(e, MIR_T_I64, "__val_rem");
        emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_SUB, emitter_reg(e, vrem_reg),
                                       emitter_reg(e, e->len_reg), emitter_reg(e, off_reg)));
        args[0] = emitter_reg(e, e->buf_reg);
        args[1] = emitter_reg(e, off_reg);
        args[2] = emitter_reg(e, vrem_reg);
        emitter_call_result(e, &e->builder->ext.read_varstr, val_reg, args, 3);
      }
      emitter_advance_reg(e, off_reg, val_total_reg);
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, skip_label),
                                     emitter_reg(e, key_reg), MIR_new_int_op(e->builder->ctx, 0)));
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, skip_label),
                                     emitter_reg(e, val_reg), MIR_new_int_op(e->builder->ctx, 0)));
      args[0] = emitter_reg(e, map_reg);
      args[1] = emitter_reg(e, key_reg);
      args[2] = emitter_reg(e, val_reg);
      emitter_call(e, &e->builder->ext.add_map_str_str, args, 3);
      emitter_append(e, skip_label);
      args[0] = emitter_reg(e, key_reg);
      emitter_call(e, &e->builder->ext.free_fn, args, 1);
      args[0] = emitter_reg(e, val_reg);
      emitter_call(e, &e->builder->ext.free_fn, args, 1);
    } else if (field->kind == EF_MAP_STR_INT || field->kind == EF_MAP_STR_DBL ||
               field->kind == EF_MAP_STR_BOOL) {
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, skip_label),
                                     emitter_reg(e, key_reg), MIR_new_int_op(e->builder->ctx, 0)));
      args[0] = emitter_reg(e, map_reg);
      args[1] = emitter_reg(e, key_reg);
      if (field->kind == EF_MAP_STR_INT) {
        emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
        args[2] = emitter_int32_value(e, field, off_reg);
        emitter_call(e, &e->builder->ext.add_map_str_int, args, 3);
        emitter_advance_const(e, off_reg, field->size);
      } else if (field->kind == EF_MAP_STR_BOOL) {
        emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
        args[2] = emitter_bool_value(e, off_reg);
        emitter_call(e, &e->builder->ext.add_map_str_bool, args, 3);
        emitter_advance_const(e, off_reg, field->size);
      } else {
        emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
        args[2] = field->mir_type == MIR_T_F ? emitter_float_to_double(e, off_reg)
                                             : emitter_mem(e, MIR_T_D, off_reg, 0);
        emitter_call(e, &e->builder->ext.add_map_str_dbl, args, 3);
        emitter_advance_const(e, off_reg, field->size);
      }
      emitter_append(e, skip_label);
      args[0] = emitter_reg(e, key_reg);
      emitter_call(e, &e->builder->ext.free_fn, args, 1);
    }
  }
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, index_reg),
                                 emitter_reg(e, index_reg), MIR_new_int_op(e->builder->ctx, 1)));
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_JMP, emitter_label(e, loop_label)));
  emitter_append(e, done_label);
  args[0] = emitter_reg(e, target_obj_reg);
  args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
  args[2] = emitter_reg(e, map_reg);
  emitter_call(e, &e->builder->ext.set_map, args, 3);
}

static void emit_group_field(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                             const emit_field_t *field, MIR_item_t field_name_item) {
  MIR_reg_t block_len_reg, count_reg, entries_size_reg, total_reg, list_reg, entries_off_reg,
      index_reg;
  MIR_label_t loop_label, done_label;
  MIR_op_t args[4];
  emitter_bounds_check_const(e, off_reg, field->group_dim, e->fail_label);
  block_len_reg = emitter_load_u16(e, off_reg, 0);
  count_reg = emitter_load_u16(e, off_reg, 2);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UBGT, emitter_label(e, e->fail_label),
                                 MIR_new_int_op(e->builder->ctx, field->size),
                                 emitter_reg(e, block_len_reg)));
  entries_size_reg = emitter_new_reg(e, MIR_T_I64, "__entries_size");
  total_reg = emitter_new_reg(e, MIR_T_I64, "__group_total");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MUL, emitter_reg(e, entries_size_reg),
                                 emitter_reg(e, block_len_reg), emitter_reg(e, count_reg)));
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, total_reg),
                                 emitter_reg(e, entries_size_reg),
                                 MIR_new_int_op(e->builder->ctx, field->group_dim)));
  emitter_bounds_check_reg(e, off_reg, total_reg, e->fail_label);
  list_reg = emitter_new_reg(e, MIR_T_I64, "__group_list");
  emitter_call_result(e, &e->builder->ext.create_list, list_reg, NULL, 0);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, e->fail_label),
                                 emitter_reg(e, list_reg), MIR_new_int_op(e->builder->ctx, 0)));
  entries_off_reg = emitter_new_reg(e, MIR_T_I64, "__entries_off");
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, entries_off_reg),
                                 emitter_reg(e, off_reg)));
  emitter_advance_const(e, entries_off_reg, field->group_dim);
  index_reg = emitter_new_reg(e, MIR_T_I64, "__group_idx");
  loop_label = MIR_new_label(e->builder->ctx);
  done_label = MIR_new_label(e->builder->ctx);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, index_reg),
                                 MIR_new_int_op(e->builder->ctx, 0)));
  emitter_append(e, loop_label);
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_UBGE, emitter_label(e, done_label),
                                 emitter_reg(e, index_reg), emitter_reg(e, count_reg)));
  {
    MIR_reg_t stride_reg = emitter_new_reg(e, MIR_T_I64, "__stride"),
              entry_off_reg = emitter_new_reg(e, MIR_T_I64, "__entry_off"),
              child_reg = emitter_new_reg(e, MIR_T_I64, "__group_child"),
              child_off_reg = emitter_new_reg(e, MIR_T_I64, "__group_child_off");
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MUL, emitter_reg(e, stride_reg),
                                   emitter_reg(e, block_len_reg), emitter_reg(e, index_reg)));
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, entry_off_reg),
                                   emitter_reg(e, entries_off_reg), emitter_reg(e, stride_reg)));
    emitter_call_result(e, &e->builder->ext.create_obj, child_reg, NULL, 0);
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, e->fail_label),
                                   emitter_reg(e, child_reg), MIR_new_int_op(e->builder->ctx, 0)));
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_MOV, emitter_reg(e, child_off_reg),
                                   emitter_reg(e, entry_off_reg)));
    emit_fields_into_object(e, child_reg, child_off_reg, &field->children);
    args[0] = emitter_reg(e, list_reg);
    args[1] = emitter_reg(e, child_reg);
    emitter_call(e, &e->builder->ext.add_list_obj, args, 2);
  }
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_ADD, emitter_reg(e, index_reg),
                                 emitter_reg(e, index_reg), MIR_new_int_op(e->builder->ctx, 1)));
  emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_JMP, emitter_label(e, loop_label)));
  emitter_append(e, done_label);
  emitter_advance_reg(e, off_reg, total_reg);
  args[0] = emitter_reg(e, target_obj_reg);
  args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
  args[2] = emitter_reg(e, list_reg);
  emitter_call(e, &e->builder->ext.set_list, args, 3);
}

static void emit_field_code(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                            const emit_field_t *field) {
  MIR_item_t field_name_item = builder_make_string_data(e->builder, field->name);
  if (field_name_item == NULL) {
    emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_JMP, emitter_label(e, e->fail_label)));
    return;
  }
  if (field->kind == EF_INT || field->kind == EF_I64 || field->kind == EF_DBL ||
      field->kind == EF_BOOL ||
      field->kind == EF_UUID ||
      field->kind == EF_STR || field->kind == EF_FIX_BYTES || field->kind == EF_VAR_BYTES)
    emit_scalar_field(e, target_obj_reg, off_reg, field, field_name_item);
  else if (field->kind == EF_LIST_INT || field->kind == EF_LIST_I64 || field->kind == EF_LIST_DBL ||
           field->kind == EF_LIST_BOOL || field->kind == EF_LIST_STR || field->kind == EF_LIST_OBJ)
    emit_list_field(e, target_obj_reg, off_reg, field, field_name_item);
  else if (field->kind == EF_SET_INT || field->kind == EF_SET_DBL ||
           field->kind == EF_SET_BOOL || field->kind == EF_SET_STR)
    emit_set_field(e, target_obj_reg, off_reg, field, field_name_item);
  else if (field->kind == EF_MAP_STR_STR || field->kind == EF_MAP_STR_INT ||
           field->kind == EF_MAP_STR_DBL || field->kind == EF_MAP_STR_BOOL)
    emit_map_field(e, target_obj_reg, off_reg, field, field_name_item);
  else if (field->kind == EF_GROUP)
    emit_group_field(e, target_obj_reg, off_reg, field, field_name_item);
}

static void emit_fields_into_object(mir_emitter_t *e, MIR_reg_t target_obj_reg, MIR_reg_t off_reg,
                                    const emit_field_array_t *fields) {
  size_t i;
  for (i = 0; i < fields->count; i++)
    emit_field_code(e, target_obj_reg, off_reg, &fields->items[i]);
}

static int generate_message_function(mir_builder_t *builder, Node *message_node, Node *schema_root,
                                     int has_set_bytes) {
  const char *message_name = get_string_val(find_child(message_node, "name"));
  Node *fields_node = find_child(message_node, "fields");
  emit_field_array_t fields = {0};
  schema_validation_context_t validation_ctx = {0};
  MIR_type_t result_type = MIR_T_P;
  MIR_var_t args[] = {{MIR_T_P, "buf", 0}, {MIR_T_I64, "len", 0}};
  char func_name[256];
  mir_emitter_t e;
  if (message_name == NULL) return 0;

  /* Validate schema structure before code generation */
  if (!validate_schema_fields(&validation_ctx, fields_node, schema_root, message_name)) {
    set_codec_error(builder->codec, "Schema validation failed for %s: %s",
                    message_name, validation_ctx.error);
    return 0;
  }

  if (!build_fields(&fields, fields_node, schema_root, NULL, has_set_bytes)) {
    emit_field_array_free(&fields);
    return 0;
  }
  if (!validate_fields_api(builder->codec, message_name, &fields)) {
    emit_field_array_free(&fields);
    return 0;
  }
  snprintf(func_name, sizeof(func_name), "parse_%s", message_name);
  e.builder = builder;
  e.func_item = MIR_new_func_arr(builder->ctx, codec_strdup(builder->codec, func_name), 1,
                                 &result_type, 2, args);
  e.func = e.func_item->u.func;
  e.buf_reg = MIR_reg(builder->ctx, "buf", e.func);
  e.len_reg = MIR_reg(builder->ctx, "len", e.func);
  e.off_reg = emitter_new_reg(&e, MIR_T_I64, "__off");
  e.obj_reg = emitter_new_reg(&e, MIR_T_I64, "__obj");
  e.fail_label = MIR_new_label(builder->ctx);
  emitter_append(&e, MIR_new_insn(builder->ctx, MIR_MOV, emitter_reg(&e, e.off_reg),
                                  MIR_new_int_op(builder->ctx, 0)));
  emitter_call_result(&e, &builder->ext.create_obj, e.obj_reg, NULL, 0);
  emitter_append(&e, MIR_new_insn(builder->ctx, MIR_BEQ, emitter_label(&e, e.fail_label),
                                  emitter_reg(&e, e.obj_reg), MIR_new_int_op(builder->ctx, 0)));
  emit_fields_into_object(&e, e.obj_reg, e.off_reg, &fields);
  emitter_append(&e, MIR_new_ret_insn(builder->ctx, 1, emitter_reg(&e, e.obj_reg)));
  emitter_append(&e, e.fail_label);
  emitter_append(&e, MIR_new_ret_insn(builder->ctx, 1, MIR_new_int_op(builder->ctx, 0)));
  MIR_finish_func(builder->ctx);
  emit_field_array_free(&fields);
  return 1;
}

static Node *parse_schema_text_to_root(const char *schema_text, size_t len,
                                       const char *path, char *error_buf,
                                       size_t error_size, DataBindError *error) {
  Node *root;
  tbe_error_t err = {0};
  if (schema_text == NULL) {
    if (error_buf != NULL && error_size > 0)
      snprintf(error_buf, error_size, "Invalid schema text");
    db_error_set(error, DATA_BIND_ERR_INVALID_ARG, path, -1, -1, "Invalid schema text");
    return NULL;
  }
  root = create_node_map(NULL);
  if (root == NULL) {
    if (error_buf != NULL && error_size > 0)
      snprintf(error_buf, error_size, "Out of memory");
    db_error_set(error, DATA_BIND_ERR_OOM, path, -1, -1, "Out of memory");
    return NULL;
  }
  if (parse_schema(schema_text, len, root, &err) != 0) {
    if (error_buf != NULL && error_size > 0)
      snprintf(error_buf, error_size, "Parse error: %s", err.message);
    db_error_set(error, DATA_BIND_ERR_PARSE, path, err.line, err.column,
                 "Parse error: %s", err.message);
    node_free(root);
    return NULL;
  }
  db_error_clear(error);
  return root;
}

static Node *load_and_parse_schema(const char *schema_path, char *error_buf,
                                   size_t error_size, DataBindError *error) {
  turbo_fs_buf_t buf;
  Node *root;
  if (schema_path == NULL) {
    if (error_buf != NULL && error_size > 0)
      snprintf(error_buf, error_size, "Invalid schema path");
    db_error_set(error, DATA_BIND_ERR_INVALID_ARG, NULL, -1, -1, "Invalid schema path");
    return NULL;
  }
  if (turbo_fs_read_file(schema_path, &buf) != 0) {
    if (error_buf != NULL && error_size > 0)
      snprintf(error_buf, error_size, "Cannot read schema: %s", schema_path);
    db_error_set(error, DATA_BIND_ERR_IO, schema_path, -1, -1,
                 "Cannot read schema: %s", schema_path);
    return NULL;
  }
  root = parse_schema_text_to_root(buf.base, buf.len, schema_path, error_buf, error_size, error);
  turbo_fs_buf_free(&buf);
  return root;
}

static void data_bind_free_contents(DataBind *codec) {
  mir_func_node_t *func_node;
  owned_alloc_node_t *alloc_node;
  if (codec == NULL) return;
  
  /* Only free func_head if this codec owns it (not from cache) */
  if (!codec->is_cloned) {
    func_node = codec->func_head;
    while (func_node != NULL) {
      mir_func_node_t *next = func_node->next;
      free(func_node->type_name);
      free(func_node);
      func_node = next;
    }
  }
  codec->func_head = NULL;
  
  alloc_node = codec->owned_allocs;
  while (alloc_node != NULL) {
    owned_alloc_node_t *next = alloc_node->next;
    free(alloc_node->ptr);
    free(alloc_node);
    alloc_node = next;
  }
  codec->owned_allocs = NULL;
  
  /* Handle MIR context based on whether this is a cloned codec */
  if (codec->ctx != NULL) {
    if (codec->is_cloned && codec->schema_hash[0] != '\0') {
      /* Release cache reference - context will be freed by cache when ref_count reaches 0 */
      mir_cache_release(codec->schema_hash);
    } else if (!codec->is_cloned && codec->schema_hash[0] == '\0') {
      /* Owned context not in cache, can be destroyed directly */
      MIR_finish(codec->ctx);
    }
    /* If is_cloned == 0 but schema_hash exists, context is in cache and will be freed by cache */
    codec->ctx = NULL;
  }
  
  if (codec->schema_root != NULL) {
    node_free(codec->schema_root);
    codec->schema_root = NULL;
  }
}

static MIR_module_t generate_parser_module(DataBind *codec) {
  mir_builder_t builder;
  Node *messages_node;
  size_t i;
  messages_node = find_child(codec->schema_root, "messages");
  if (messages_node == NULL || messages_node->type != NODE_LIST ||
      messages_node->data.list.count == 0) {
    set_codec_error(codec, "No messages found in schema");
    return NULL;
  }
  codec->ctx = MIR_init();
  if (codec->ctx == NULL) return NULL;
  builder.codec = codec;
  builder.ctx = codec->ctx;
  builder.module = MIR_new_module(codec->ctx, "data_bind_binary");
  builder.temp_name_id = 0;
  builder.data_name_id = 0;
  init_externals(&builder);
  for (i = 0; i < messages_node->data.list.count; i++)
    if (!generate_message_function(&builder, messages_node->data.list.items[i], codec->schema_root,
                                   codec->api.set_field_bytes != NULL)) {
      if (codec->error[0] == '\0') set_codec_error(codec, "Failed to generate parser function");
      MIR_finish(codec->ctx);
      codec->ctx = NULL;
      return NULL;
    }
  MIR_finish_module(codec->ctx);
  return builder.module;
}

static int link_module(DataBind *codec, MIR_module_t module) {
  MIR_load_module(codec->ctx, module);
  MIR_load_external(codec->ctx, "create_obj", codec->api.create_object);
  MIR_load_external(codec->ctx, "set_int", codec->api.set_field_int);
  MIR_load_external(codec->ctx, "set_int64",
                    codec->api.set_field_int64 != NULL ? codec->api.set_field_int64 : set_i64_noop);
  MIR_load_external(codec->ctx, "set_dbl", codec->api.set_field_double);
  MIR_load_external(codec->ctx, "set_bool",
                    codec->api.set_field_bool != NULL ? codec->api.set_field_bool
                                                      : set_bool_noop);
  MIR_load_external(codec->ctx, "set_str", codec->api.set_field_string);
  MIR_load_external(codec->ctx, "set_bytes",
                    codec->api.set_field_bytes != NULL ? codec->api.set_field_bytes
                                                       : set_bytes_noop);
  MIR_load_external(codec->ctx, "set_uuid",
                    codec->api.set_field_uuid != NULL ? codec->api.set_field_uuid
                                                      : set_uuid_noop);
  MIR_load_external(codec->ctx, "create_list",
                    codec->api.create_list != NULL ? codec->api.create_list : container_noop);
  MIR_load_external(codec->ctx, "add_list_int",
                    codec->api.add_list_item_int != NULL ? codec->api.add_list_item_int
                                                         : add_i32_noop);
  MIR_load_external(codec->ctx, "add_list_int64",
                    codec->api.add_list_item_int64 != NULL ? codec->api.add_list_item_int64
                                                           : add_i64_noop);
  MIR_load_external(codec->ctx, "add_list_dbl",
                    codec->api.add_list_item_double != NULL ? codec->api.add_list_item_double
                                                            : add_dbl_noop);
  MIR_load_external(codec->ctx, "add_list_bool",
                    codec->api.add_list_item_bool != NULL ? codec->api.add_list_item_bool
                                                          : add_bool_noop);
  MIR_load_external(codec->ctx, "add_list_str",
                    codec->api.add_list_item_string != NULL ? codec->api.add_list_item_string
                                                            : add_str_noop);
  MIR_load_external(codec->ctx, "add_list_obj",
                    codec->api.add_list_item_object != NULL ? codec->api.add_list_item_object
                                                            : add_obj_noop);
  MIR_load_external(codec->ctx, "set_list",
                    codec->api.set_field_list != NULL ? codec->api.set_field_list
                                                      : set_container_noop);
  MIR_load_external(codec->ctx, "create_set",
                    codec->api.create_set != NULL ? codec->api.create_set : container_noop);
  MIR_load_external(codec->ctx, "add_set_int",
                    codec->api.add_set_item_int != NULL ? codec->api.add_set_item_int
                                                        : add_i32_noop);
  MIR_load_external(codec->ctx, "add_set_dbl",
                    codec->api.add_set_item_double != NULL ? codec->api.add_set_item_double
                                                           : add_dbl_noop);
  MIR_load_external(codec->ctx, "add_set_bool",
                    codec->api.add_set_item_bool != NULL ? codec->api.add_set_item_bool
                                                         : add_bool_noop);
  MIR_load_external(codec->ctx, "add_set_str",
                    codec->api.add_set_item_string != NULL ? codec->api.add_set_item_string
                                                           : add_str_noop);
  MIR_load_external(codec->ctx, "set_set",
                    codec->api.set_field_set != NULL ? codec->api.set_field_set
                                                     : set_container_noop);
  MIR_load_external(codec->ctx, "create_map",
                    codec->api.create_map != NULL ? codec->api.create_map : container_noop);
  MIR_load_external(codec->ctx, "add_map_str_str",
                    codec->api.add_map_entry_string_string != NULL
                        ? codec->api.add_map_entry_string_string
                        : add_map_str_str_noop);
  MIR_load_external(codec->ctx, "add_map_str_int",
                    codec->api.add_map_entry_string_int != NULL
                        ? codec->api.add_map_entry_string_int
                        : add_map_str_int_noop);
  MIR_load_external(codec->ctx, "add_map_str_dbl",
                    codec->api.add_map_entry_string_double != NULL
                        ? codec->api.add_map_entry_string_double
                        : add_map_str_dbl_noop);
  MIR_load_external(codec->ctx, "add_map_str_bool",
                    codec->api.add_map_entry_string_bool != NULL
                        ? codec->api.add_map_entry_string_bool
                        : add_map_str_bool_noop);
  MIR_load_external(codec->ctx, "set_map",
                    codec->api.set_field_map != NULL ? codec->api.set_field_map
                                                     : set_container_noop);
  MIR_load_external(codec->ctx, "read_varstr", tbe_read_varstring);
  MIR_load_external(codec->ctx, "free", free);
  MIR_gen_init(codec->ctx);
  MIR_link(codec->ctx, MIR_set_gen_interface, NULL);
  return 1;
}

static void register_parse_functions(DataBind *codec, MIR_module_t module) {
  Node *messages_node = find_child(codec->schema_root, "messages");
  size_t i;
  if (messages_node == NULL || messages_node->type != NODE_LIST) return;
  for (i = 0; i < messages_node->data.list.count; i++) {
    Node *msg = messages_node->data.list.items[i];
    const char *msg_name = get_string_val(find_child(msg, "name"));
    char func_name[256];
    MIR_item_t item;
    if (msg_name == NULL) continue;
    snprintf(func_name, sizeof(func_name), "parse_%s", msg_name);
    for (item = DLIST_HEAD(MIR_item_t, module->items); item != NULL;
         item = DLIST_NEXT(MIR_item_t, item)) {
      if (item->item_type == MIR_func_item && strcmp(item->u.func->name, func_name) == 0) {
        mir_func_node_t *node = (mir_func_node_t *)malloc(sizeof(*node));
        if (node == NULL) return;
        /* type_name is freed individually in data_bind_free; not tracked by codec_alloc */
        node->type_name = strdup(msg_name);
        node->parse_fn = item->addr;
        node->next = codec->func_head;
        codec->func_head = node;
        break;
      }
    }
  }
}

static DataBindStatus data_bind_finish_codec(DataBind *codec, DataBindError *error,
                                             const char *schema_text, size_t schema_len) {
  MIR_module_t module;
  mir_cache_entry_t *cache_entry = NULL;
  
  /* Compute schema hash for caching */
  if (schema_text != NULL && schema_len > 0) {
    compute_schema_hash(schema_text, schema_len, codec->schema_hash);
    cache_entry = mir_cache_find(codec->schema_hash);
  }
  
  /* Use cached MIR context if available */
  if (cache_entry != NULL && cache_entry->shared_ctx != NULL) {
    codec->ctx = cache_entry->shared_ctx;
    codec->func_head = cache_entry->func_head;
    codec->is_cloned = 1;
    cache_entry->ref_count++;
    db_error_clear(error);
    codec->error[0] = '\0';
    codec->binary_error[0] = '\0';
    return DATA_BIND_OK;
  }
  
  /* Generate new MIR module */
  module = generate_parser_module(codec);
  if (module == NULL) {
    snprintf(codec->binary_error, sizeof(codec->binary_error), "%s",
             codec->error[0] != '\0' ? codec->error : "Failed to generate parser module");
    codec->error[0] = '\0';
    db_error_clear(error);
    return DATA_BIND_OK;
  }
  if (!link_module(codec, module)) {
    snprintf(codec->binary_error, sizeof(codec->binary_error), "%s",
             codec->error[0] != '\0' ? codec->error : "Failed to link parser module");
    codec->error[0] = '\0';
    db_error_clear(error);
    return DATA_BIND_OK;
  }
  register_parse_functions(codec, module);
  
  /* Cache the MIR context if hashing was successful */
  if (codec->schema_hash[0] != '\0') {
    cache_entry = mir_cache_insert(codec->schema_hash, codec->ctx, codec->func_head);
    if (cache_entry != NULL) {
      codec->is_cloned = 1;  /* Mark as using cached context */
    } else {
      codec->is_cloned = 0;  /* Cache failed, codec owns the context */
    }
  } else {
    codec->is_cloned = 0;  /* No hash, codec owns the context */
  }
  
  db_error_clear(error);
  codec->error[0] = '\0';
  codec->binary_error[0] = '\0';
  return DATA_BIND_OK;
}

static DataBindStatus data_bind_create_with_api_from_root(Node *schema_root,
                                                          const data_bind_runtime_api_t *api,
                                                          DataBind **out_codec,
                                                          DataBindError *error,
                                                          const char *schema_text,
                                                          size_t schema_len) {
  DataBind *codec;
  DataBindStatus status;
  if (out_codec != NULL) *out_codec = NULL;
  if (api == NULL || api->create_object == NULL || api->set_field_int == NULL ||
      api->set_field_double == NULL || api->set_field_string == NULL)
    return db_error_set(error, DATA_BIND_ERR_INVALID_ARG, NULL, -1, -1,
                        "Invalid runtime API");
  if (schema_root == NULL || out_codec == NULL)
    return db_error_set(error, DATA_BIND_ERR_INVALID_ARG, NULL, -1, -1,
                        "Invalid codec create arguments");
  codec = (DataBind *)calloc(1, sizeof(*codec));
  if (codec == NULL) {
    node_free(schema_root);
    return db_error_set(error, DATA_BIND_ERR_OOM, NULL, -1, -1, "Out of memory");
  }
  codec->api = *api;
  codec->schema_root = schema_root;
  codec->is_cloned = 0;
  codec->schema_hash[0] = '\0';
  status = data_bind_finish_codec(codec, error, schema_text, schema_len);
  if (status != DATA_BIND_OK) {
    data_bind_free(codec);
    return status;
  }
  *out_codec = codec;
  return DATA_BIND_OK;
}

static DataBindStatus data_bind_create_with_api(const char *schema_path,
                                                const data_bind_runtime_api_t *api,
                                                DataBind **out_codec,
                                                DataBindError *error) {
  Node *schema_root;
  turbo_fs_buf_t schema_buf = {NULL, 0};
  DataBindStatus status;
  
  if (out_codec != NULL) *out_codec = NULL;
  
  /* Load schema file to get both parsed AST and raw text for hashing */
  if (turbo_fs_read_file(schema_path, &schema_buf) != 0) {
    return db_error_set(error, DATA_BIND_ERR_IO, schema_path, -1, -1,
                       "Cannot read schema: %s", schema_path);
  }
  
  schema_root = parse_schema_text_to_root(schema_buf.base, schema_buf.len,
                                          schema_path, NULL, 0, error);
  if (schema_root == NULL) {
    turbo_fs_buf_free(&schema_buf);
    return error != NULL && error->code != DATA_BIND_OK ? error->code : DATA_BIND_ERR_SCHEMA;
  }
  
  status = data_bind_create_with_api_from_root(schema_root, api, out_codec, error,
                                                schema_buf.base, schema_buf.len);
  turbo_fs_buf_free(&schema_buf);
  return status;
}

DataBindStatus data_bind_create(const char *schema_path, DataBind **out_codec,
                                DataBindError *error) {
  return data_bind_create_with_api(schema_path, &DYNAMIC_VALUE_API, out_codec, error);
}

DataBindStatus data_bind_create_from_text(const char *schema_text, size_t len,
                                          DataBind **out_codec, DataBindError *error) {
  Node *schema_root;
  if (out_codec != NULL) *out_codec = NULL;
  schema_root = parse_schema_text_to_root(schema_text, len, NULL, NULL, 0, error);
  if (schema_root == NULL)
    return error != NULL && error->code != DATA_BIND_OK ? error->code : DATA_BIND_ERR_SCHEMA;
  return data_bind_create_with_api_from_root(schema_root, &DYNAMIC_VALUE_API, out_codec, error,
                                              schema_text, len);
}

void data_bind_free(DataBind *codec) {
  if (codec == NULL) return;
  data_bind_free_contents(codec);
  free(codec);
}

void data_bind_set_cache_enabled(int enabled) {
  g_mir_cache_enabled = enabled != 0;
}

void data_bind_clear_cache(void) {
  mir_cache_entry_t *entry = g_mir_cache_head;
  mir_cache_entry_t *next;
  
  while (entry != NULL) {
    next = entry->next;
    /* Only free entries with zero ref count */
    if (entry->ref_count <= 0) {
      if (entry->shared_ctx != NULL) {
        MIR_finish(entry->shared_ctx);
      }
      free(entry);
    }
    entry = next;
  }
  
  /* Rebuild list with only referenced entries */
  g_mir_cache_head = NULL;
  entry = g_mir_cache_head;
  while (entry != NULL) {
    if (entry->ref_count > 0) {
      entry->next = g_mir_cache_head;
      g_mir_cache_head = entry;
    }
    entry = entry->next;
  }
}

void data_bind_set_value_pool_enabled(int enabled) {
  g_value_pool_enabled = enabled != 0;
  
  /* If disabling, free all pooled nodes */
  if (!g_value_pool_enabled) {
    DataBindValue *node = g_value_pool.free_list;
    while (node != NULL) {
      DataBindValue *next = (DataBindValue *)node->data.object_val.items;
      free(node);
      node = next;
    }
    g_value_pool.free_list = NULL;
  }
}

void data_bind_get_value_pool_stats(size_t *allocated, size_t *reused) {
  if (allocated != NULL) *allocated = g_value_pool.allocated_count;
  if (reused != NULL) *reused = g_value_pool.reused_count;
}

static DataBindStatus data_bind_emit_file_to_writer(FILE *file, DataBindWriteFn write,
                                                    void *user, DataBindError *error) {
  unsigned char buf[4096];
  size_t n;
  if (fflush(file) != 0 || fseek(file, 0, SEEK_SET) != 0)
    return db_error_set(error, DATA_BIND_ERR_IO, NULL, -1, -1, "Failed to rewind MIR output");
  while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
    if (write(buf, n, user) != 0)
      return db_error_set(error, DATA_BIND_ERR_IO, NULL, -1, -1, "MIR write callback failed");
  }
  if (ferror(file))
    return db_error_set(error, DATA_BIND_ERR_IO, NULL, -1, -1, "Failed to read MIR output");
  return DATA_BIND_OK;
}

DataBindStatus data_bind_generate_mir(const char *schema_path, DataBindWriteFn write,
                                      void *user, int binary_output, DataBindError *error) {
  DataBind codec;
  MIR_module_t module = NULL;
  FILE *tmp = NULL;
  DataBindStatus status = DATA_BIND_ERR_RUNTIME;

  db_error_clear(error);
  if (schema_path == NULL || write == NULL)
    return db_error_set(error, DATA_BIND_ERR_INVALID_ARG, schema_path, -1, -1,
                        "Invalid MIR output arguments");

  memset(&codec, 0, sizeof(codec));
  codec.api = MIR_OUTPUT_API;
  codec.schema_root = load_and_parse_schema(schema_path, codec.error, sizeof(codec.error), error);
  if (codec.schema_root == NULL) goto cleanup;

  module = generate_parser_module(&codec);
  if (module == NULL) {
    status = db_error_set(error, DATA_BIND_ERR_SCHEMA, schema_path, -1, -1, "%s",
                          codec.error[0] != '\0' ? codec.error : "MIR generation failed");
    goto cleanup;
  }

  tmp = tmpfile();
  if (tmp == NULL) {
    status = db_error_set(error, DATA_BIND_ERR_IO, schema_path, -1, -1,
                          "Failed to create temporary MIR output");
    goto cleanup;
  }
  if (binary_output)
    MIR_write_module(codec.ctx, tmp, module);
  else
    MIR_output_module(codec.ctx, tmp, module);
  if (ferror(tmp)) {
    status = db_error_set(error, DATA_BIND_ERR_IO, schema_path, -1, -1, "MIR output failed");
    goto cleanup;
  }
  status = data_bind_emit_file_to_writer(tmp, write, user, error);

cleanup:
  if (tmp != NULL) fclose(tmp);
  data_bind_free_contents(&codec);
  return status;
}

DataBindStatus data_bind_parse(DataBind *codec, const char *type_name, const uint8_t *buf,
                               size_t len, DataBindValue **out_value, DataBindError *error) {
  mir_func_node_t *node;
  DataBindValue *(*parse_fn)(const uint8_t *, int64_t);
  DataBindValue *result;
  char error_path[64];
  if (out_value != NULL) *out_value = NULL;
  if (codec == NULL || type_name == NULL || buf == NULL || out_value == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid binary bind arguments");
  if (codec->func_head == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "binary", NULL);
    return db_error_set(error, DATA_BIND_ERR_RUNTIME, error_path, -1, -1, "%s",
                        codec->binary_error[0] != '\0'
                            ? codec->binary_error
                            : "Binary parser is unavailable for this schema");
  }
  for (node = codec->func_head; node != NULL; node = node->next) {
    if (strcmp(node->type_name, type_name) == 0) {
      parse_fn = (DataBindValue *(*)(const uint8_t *, int64_t))node->parse_fn;
      codec->error[0] = '\0';
      result = parse_fn(buf, (int64_t)len);
      if (result == NULL) {
        db_error_format_path(error_path, sizeof(error_path), "binary", "parse failed");
        return db_error_set(error, DATA_BIND_ERR_PARSE, error_path, -1, -1,
                            "Binary bind failed for type: %s", type_name);
      }
      *out_value = result;
      db_error_clear(error);
      return DATA_BIND_OK;
    }
  }
  return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_NOT_FOUND,
                        "Type not found: %s", type_name);
}

DataBindStatus data_bind_parse_json(DataBind *codec, const char *type_name,
                                    const char *json, size_t len,
                                    DataBindValue **out_value, DataBindError *error) {
  json_value_t *root = NULL;
  DataBindValue *result;
  void *ptr;
  char error_path[128];
  if (out_value != NULL) *out_value = NULL;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || json == NULL ||
      out_value == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid JSON bind arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    db_error_format_path(error_path, sizeof(error_path), "json", NULL);
    return db_error_set(error, DATA_BIND_ERR_TYPE_NOT_FOUND, error_path, -1, -1,
                        "Type not found: %s", type_name);
  }
  if (turbo_parse_json((const uint8_t *)json, len, &root) != 0 || root == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "json", NULL);
    return db_error_set(error, DATA_BIND_ERR_PARSE, error_path, -1, -1, "JSON parse failed");
  }
  result = bind_json_typed_value(codec->schema_root, type_name, root);
  ptr = root;
  turbo_free_json(&ptr);
  if (result == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "json", "$");
    return db_error_set(error, DATA_BIND_ERR_TYPE_MISMATCH, error_path, -1, -1,
                        "JSON bind failed for type: %s", type_name);
  }
  *out_value = result;
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_parse_json_all(DataBind *codec, const char *type_name,
                                        const char *json, size_t len,
                                        DataBindValue **out_value, DataBindError *error) {
  json_value_t *root = NULL;
  DataBindValue *list;
  void *ptr;
  size_t i;
  char error_path[128];
  if (out_value != NULL) *out_value = NULL;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || json == NULL ||
      out_value == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid JSON bind_all arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    db_error_format_path(error_path, sizeof(error_path), "json", NULL);
    return db_error_set(error, DATA_BIND_ERR_TYPE_NOT_FOUND, error_path, -1, -1,
                        "Type not found: %s", type_name);
  }
  if (turbo_parse_json((const uint8_t *)json, len, &root) != 0 || root == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "json", NULL);
    return db_error_set(error, DATA_BIND_ERR_PARSE, error_path, -1, -1, "JSON parse failed");
  }
  list = dbv_new(DATA_BIND_VALUE_LIST);
  if (list != NULL) {
    if (turbo_json_type(root) == TURBO_JSON_ARRAY) {
      for (i = 0; i < turbo_json_array_size(root); i++) {
        DataBindValue *item = bind_json_typed_value(codec->schema_root, type_name,
                                                    turbo_json_array_get(root, i));
        if (item != NULL) {
          if (!dbv_array_push(&list->data.array_val, item)) {
            data_bind_value_free(item);
            data_bind_value_free(list);
            list = NULL;
            break;
          }
        }
      }
    } else {
      DataBindValue *item = bind_json_typed_value(codec->schema_root, type_name, root);
      if (item == NULL || !dbv_array_push(&list->data.array_val, item)) {
        data_bind_value_free(item);
        data_bind_value_free(list);
        list = NULL;
      }
    }
  }
  ptr = root;
  turbo_free_json(&ptr);
  if (list == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "json", "$[]");
    return db_error_set(error, DATA_BIND_ERR_TYPE_MISMATCH, error_path, -1, -1,
                        "JSON bind_all failed for type: %s", type_name);
  }
  *out_value = list;
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_parse_csv(DataBind *codec, const char *type_name,
                                   const char *csv, size_t len, size_t row,
                                   DataBindValue **out_value, DataBindError *error) {
  turbo_csv_doc_t *doc = NULL;
  data_bind_csv_headers_t headers = {0};
  turbo_csv_options_t opts = {true, ',', '"', true};
  DataBindValue *result = NULL;
  void *ptr;
  char error_path[128];
  if (out_value != NULL) *out_value = NULL;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || csv == NULL ||
      out_value == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid CSV bind arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    db_error_format_path(error_path, sizeof(error_path), "csv", NULL);
    return db_error_set(error, DATA_BIND_ERR_TYPE_NOT_FOUND, error_path, -1, -1,
                        "Type not found: %s", type_name);
  }
  if (turbo_parse_csv_opts((const uint8_t *)csv, len, &opts, &doc) != 0 || doc == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "csv", NULL);
    return db_error_set(error, DATA_BIND_ERR_PARSE, error_path, -1, -1, "CSV parse failed");
  }
  if (csv_parse_header_names(csv, len, &headers))
    result = bind_csv_typed_value(codec->schema_root, type_name, doc, row, &headers, "");
  csv_headers_free(&headers);
  ptr = doc;
  turbo_free_csv(&ptr);
  if (result == NULL) {
    snprintf(error_path, sizeof(error_path), "csv: row %zu", row);
    return db_error_set(error, DATA_BIND_ERR_TYPE_MISMATCH, error_path, (int)row, -1,
                        "CSV bind failed for type: %s", type_name);
  }
  *out_value = result;
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_parse_csv_all(DataBind *codec, const char *type_name,
                                       const char *csv, size_t len,
                                       DataBindValue **out_value, DataBindError *error) {
  turbo_csv_doc_t *doc = NULL;
  data_bind_csv_headers_t headers = {0};
  turbo_csv_options_t opts = {true, ',', '"', true};
  DataBindValue *list = NULL;
  void *ptr;
  size_t row;
  char error_path[128];
  if (out_value != NULL) *out_value = NULL;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || csv == NULL ||
      out_value == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid CSV bind_all arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    db_error_format_path(error_path, sizeof(error_path), "csv", NULL);
    return db_error_set(error, DATA_BIND_ERR_TYPE_NOT_FOUND, error_path, -1, -1,
                        "Type not found: %s", type_name);
  }
  if (turbo_parse_csv_opts((const uint8_t *)csv, len, &opts, &doc) != 0 || doc == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "csv", NULL);
    return db_error_set(error, DATA_BIND_ERR_PARSE, error_path, -1, -1, "CSV parse failed");
  }
  if (csv_parse_header_names(csv, len, &headers)) {
    list = dbv_new(DATA_BIND_VALUE_LIST);
    if (list != NULL) {
      for (row = 0; row < turbo_csv_row_count(doc); row++) {
        DataBindValue *item = bind_csv_typed_value(codec->schema_root, type_name, doc, row, &headers, "");
        if (item != NULL) {
          if (!dbv_array_push(&list->data.array_val, item)) {
            data_bind_value_free(item);
            data_bind_value_free(list);
            list = NULL;
            break;
          }
        }
      }
    }
  }
  csv_headers_free(&headers);
  ptr = doc;
  turbo_free_csv(&ptr);
  if (list == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "csv", "multiple rows");
    return db_error_set(error, DATA_BIND_ERR_TYPE_MISMATCH, error_path, -1, -1,
                        "CSV bind_all failed for type: %s", type_name);
  }
  *out_value = list;
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_parse_xml(DataBind *codec, const char *type_name,
                                   const char *xml, size_t len,
                                   DataBindValue **out_value, DataBindError *error) {
  turbo_xml_doc_t *doc = NULL;
  DataBindValue *result = NULL;
  void *ptr;
  char error_path[128];
  if (out_value != NULL) *out_value = NULL;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || xml == NULL ||
      out_value == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid XML bind arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    db_error_format_path(error_path, sizeof(error_path), "xml", NULL);
    return db_error_set(error, DATA_BIND_ERR_TYPE_NOT_FOUND, error_path, -1, -1,
                        "Type not found: %s", type_name);
  }
  if (turbo_parse_xml((const uint8_t *)xml, len, &doc) != 0 || doc == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "xml", NULL);
    return db_error_set(error, DATA_BIND_ERR_PARSE, error_path, -1, -1, "XML parse failed");
  }
  result = bind_xml_typed_value(codec->schema_root, type_name, doc, "/*");
  ptr = doc;
  turbo_free_xml(&ptr);
  if (result == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "xml", "/*");
    return db_error_set(error, DATA_BIND_ERR_TYPE_MISMATCH, error_path, -1, -1,
                        "XML bind failed for type: %s", type_name);
  }
  *out_value = result;
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_parse_xml_all(DataBind *codec, const char *type_name,
                                       const char *xml, size_t len, const char *xpath,
                                       DataBindValue **out_value, DataBindError *error) {
  turbo_xml_doc_t *doc = NULL;
  DataBindValue *list = NULL;
  void *ptr;
  char error_path[256];
  if (out_value != NULL) *out_value = NULL;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || xml == NULL ||
      out_value == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid XML bind_all arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    db_error_format_path(error_path, sizeof(error_path), "xml", NULL);
    return db_error_set(error, DATA_BIND_ERR_TYPE_NOT_FOUND, error_path, -1, -1,
                        "Type not found: %s", type_name);
  }
  if (turbo_parse_xml((const uint8_t *)xml, len, &doc) != 0 || doc == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "xml", NULL);
    return db_error_set(error, DATA_BIND_ERR_PARSE, error_path, -1, -1, "XML parse failed");
  }
  list = dbv_new(DATA_BIND_VALUE_LIST);
  if (list != NULL) {
    if (xpath == NULL || xpath[0] == '\0') {
      DataBindValue *item = bind_xml_typed_value(codec->schema_root, type_name, doc, "/*");
      if (item == NULL || !dbv_array_push(&list->data.array_val, item)) {
        data_bind_value_free(item);
        data_bind_value_free(list);
        list = NULL;
      }
    } else {
      turbo_xml_list_t nodes;
      int index = 0;
      turbo_xml_xpath_query(doc, xpath, &nodes);
      for (index = 0; index < nodes.len; index++) {
        char item_path[320];
        DataBindValue *item;
        if (snprintf(item_path, sizeof(item_path), "%s[%d]", xpath, index + 1) >=
            (int)sizeof(item_path)) {
          data_bind_value_free(list);
          list = NULL;
          break;
        }
        item = bind_xml_typed_value(codec->schema_root, type_name, doc, item_path);
        if (item != NULL) {
          if (!dbv_array_push(&list->data.array_val, item)) {
            data_bind_value_free(item);
            data_bind_value_free(list);
            list = NULL;
            break;
          }
        }
      }
      turbo_xml_list_free(&nodes);
    }
  }
  ptr = doc;
  turbo_free_xml(&ptr);
  if (list == NULL) {
    db_error_format_path(error_path, sizeof(error_path), "xml", 
                         xpath != NULL && xpath[0] != '\0' ? xpath : "/*");
    return db_error_set(error, DATA_BIND_ERR_TYPE_MISMATCH, error_path, -1, -1,
                        "XML bind_all failed for type: %s", type_name);
  }
  *out_value = list;
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_validate_json(DataBind *codec, const char *type_name,
                                       const char *json, size_t len,
                                       DataBindError *error) {
  json_value_t *root = NULL;
  void *ptr;
  size_t i;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || json == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid JSON validate arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_NOT_FOUND,
                          "Type not found: %s", type_name);
  }
  if (turbo_parse_json((const uint8_t *)json, len, &root) != 0 || root == NULL) {
    return db_codec_error(codec, error, DATA_BIND_ERR_PARSE, "JSON parse failed");
  }
  if (turbo_json_type(root) == TURBO_JSON_ARRAY) {
    for (i = 0; i < turbo_json_array_size(root); i++) {
      DataBindValue *item = bind_json_typed_value(codec->schema_root, type_name,
                                                  turbo_json_array_get(root, i));
      if (item == NULL) {
        ptr = root;
        turbo_free_json(&ptr);
        return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_MISMATCH,
                              "JSON validation failed for type: %s", type_name);
      }
      data_bind_value_free(item);
    }
  } else {
    DataBindValue *item = bind_json_typed_value(codec->schema_root, type_name, root);
    if (item == NULL) {
      ptr = root;
      turbo_free_json(&ptr);
      return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_MISMATCH,
                            "JSON validation failed for type: %s", type_name);
    }
    data_bind_value_free(item);
  }
  ptr = root;
  turbo_free_json(&ptr);
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_validate_csv(DataBind *codec, const char *type_name,
                                      const char *csv, size_t len,
                                      DataBindError *error) {
  turbo_csv_doc_t *doc = NULL;
  data_bind_csv_headers_t headers = {0};
  turbo_csv_options_t opts = {true, ',', '"', true};
  void *ptr;
  size_t row;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || csv == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid CSV validate arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_NOT_FOUND,
                          "Type not found: %s", type_name);
  }
  if (turbo_parse_csv_opts((const uint8_t *)csv, len, &opts, &doc) != 0 || doc == NULL) {
    return db_codec_error(codec, error, DATA_BIND_ERR_PARSE, "CSV parse failed");
  }
  if (!csv_parse_header_names(csv, len, &headers)) {
    ptr = doc;
    turbo_free_csv(&ptr);
    return db_codec_error(codec, error, DATA_BIND_ERR_PARSE, "CSV header parse failed");
  }
  for (row = 0; row < turbo_csv_row_count(doc); row++) {
    DataBindValue *item = bind_csv_typed_value(codec->schema_root, type_name, doc, row, &headers, "");
    if (item == NULL) {
      csv_headers_free(&headers);
      ptr = doc;
      turbo_free_csv(&ptr);
      return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_MISMATCH,
                            "CSV validation failed for type: %s", type_name);
    }
    data_bind_value_free(item);
  }
  csv_headers_free(&headers);
  ptr = doc;
  turbo_free_csv(&ptr);
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindStatus data_bind_validate_xml(DataBind *codec, const char *type_name,
                                      const char *xml, size_t len, const char *xpath,
                                      DataBindError *error) {
  turbo_xml_doc_t *doc = NULL;
  void *ptr;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || xml == NULL)
    return db_codec_error(codec, error, DATA_BIND_ERR_INVALID_ARG,
                          "Invalid XML validate arguments");
  codec->error[0] = '\0';
  if (!bind_type_supported(codec->schema_root, type_name)) {
    return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_NOT_FOUND,
                          "Type not found: %s", type_name);
  }
  if (turbo_parse_xml((const uint8_t *)xml, len, &doc) != 0 || doc == NULL) {
    return db_codec_error(codec, error, DATA_BIND_ERR_PARSE, "XML parse failed");
  }
  if (xpath == NULL || xpath[0] == '\0') {
    DataBindValue *item = bind_xml_typed_value(codec->schema_root, type_name, doc, "/*");
    if (item == NULL) {
      ptr = doc;
      turbo_free_xml(&ptr);
      return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_MISMATCH,
                            "XML validation failed for type: %s", type_name);
    }
    data_bind_value_free(item);
  } else {
    turbo_xml_list_t nodes;
    int index;
    turbo_xml_xpath_query(doc, xpath, &nodes);
    for (index = 0; index < nodes.len; index++) {
      char item_path[320];
      DataBindValue *item;
      if (snprintf(item_path, sizeof(item_path), "%s[%d]", xpath, index + 1) >=
          (int)sizeof(item_path)) {
        turbo_xml_list_free(&nodes);
        ptr = doc;
        turbo_free_xml(&ptr);
        return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_MISMATCH,
                              "XML validation path is too long");
      }
      item = bind_xml_typed_value(codec->schema_root, type_name, doc, item_path);
      if (item == NULL) {
        turbo_xml_list_free(&nodes);
        ptr = doc;
        turbo_free_xml(&ptr);
        return db_codec_error(codec, error, DATA_BIND_ERR_TYPE_MISMATCH,
                              "XML validation failed for type: %s", type_name);
      }
      data_bind_value_free(item);
    }
    turbo_xml_list_free(&nodes);
  }
  ptr = doc;
  turbo_free_xml(&ptr);
  db_error_clear(error);
  return DATA_BIND_OK;
}

DataBindValueKind data_bind_value_kind(const DataBindValue *value) {
  return value != NULL ? value->kind : DATA_BIND_VALUE_NULL;
}

size_t data_bind_value_field_count(const DataBindValue *value) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_OBJECT) return 0;
  return value->data.object_val.count;
}

const char *data_bind_value_field_name(const DataBindValue *value, size_t index) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_OBJECT ||
      index >= value->data.object_val.count)
    return NULL;
  return value->data.object_val.items[index].name;
}

const DataBindValue *data_bind_value_field_at(const DataBindValue *value, size_t index) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_OBJECT ||
      index >= value->data.object_val.count)
    return NULL;
  return value->data.object_val.items[index].value;
}

const DataBindValue *data_bind_value_get(const DataBindValue *value, const char *name) {
  size_t i;
  if (value == NULL || value->kind != DATA_BIND_VALUE_OBJECT || name == NULL) return NULL;
  for (i = 0; i < value->data.object_val.count; i++)
    if (strcmp(value->data.object_val.items[i].name, name) == 0)
      return value->data.object_val.items[i].value;
  return NULL;
}

size_t data_bind_value_count(const DataBindValue *value) {
  if (value == NULL) return 0;
  if (value->kind == DATA_BIND_VALUE_LIST || value->kind == DATA_BIND_VALUE_SET)
    return value->data.array_val.count;
  if (value->kind == DATA_BIND_VALUE_MAP) return value->data.map_val.count;
  return 0;
}

const DataBindValue *data_bind_value_at(const DataBindValue *value, size_t index) {
  if (value == NULL ||
      (value->kind != DATA_BIND_VALUE_LIST && value->kind != DATA_BIND_VALUE_SET) ||
      index >= value->data.array_val.count)
    return NULL;
  return value->data.array_val.items[index];
}

DataBindMapEntry data_bind_value_map_entry_at(const DataBindValue *value, size_t index) {
  DataBindMapEntry entry;
  entry.key = NULL;
  entry.value = NULL;
  if (value == NULL || value->kind != DATA_BIND_VALUE_MAP || index >= value->data.map_val.count)
    return entry;
  entry.key = value->data.map_val.items[index].key;
  entry.value = value->data.map_val.items[index].value;
  return entry;
}

int32_t data_bind_value_as_int(const DataBindValue *value) {
  if (value == NULL) return 0;
  if (value->kind == DATA_BIND_VALUE_INT) return value->data.int_val;
  if (value->kind == DATA_BIND_VALUE_INT64) return (int32_t)value->data.int64_val;
  if (value->kind == DATA_BIND_VALUE_DOUBLE) return (int32_t)value->data.double_val;
  if (value->kind == DATA_BIND_VALUE_BOOL) return value->data.bool_val ? 1 : 0;
  return 0;
}

int64_t data_bind_value_as_int64(const DataBindValue *value) {
  if (value == NULL) return 0;
  if (value->kind == DATA_BIND_VALUE_INT64) return value->data.int64_val;
  if (value->kind == DATA_BIND_VALUE_INT) return value->data.int_val;
  if (value->kind == DATA_BIND_VALUE_DOUBLE) return (int64_t)value->data.double_val;
  if (value->kind == DATA_BIND_VALUE_BOOL) return value->data.bool_val ? 1 : 0;
  return 0;
}

double data_bind_value_as_double(const DataBindValue *value) {
  if (value == NULL) return 0.0;
  if (value->kind == DATA_BIND_VALUE_DOUBLE) return value->data.double_val;
  if (value->kind == DATA_BIND_VALUE_INT64) return (double)value->data.int64_val;
  if (value->kind == DATA_BIND_VALUE_INT) return (double)value->data.int_val;
  if (value->kind == DATA_BIND_VALUE_BOOL) return value->data.bool_val ? 1.0 : 0.0;
  return 0.0;
}

int data_bind_value_as_bool(const DataBindValue *value) {
  if (value == NULL) return 0;
  if (value->kind == DATA_BIND_VALUE_BOOL) return value->data.bool_val != 0;
  if (value->kind == DATA_BIND_VALUE_INT) return value->data.int_val != 0;
  if (value->kind == DATA_BIND_VALUE_INT64) return value->data.int64_val != 0;
  if (value->kind == DATA_BIND_VALUE_DOUBLE) return value->data.double_val != 0.0;
  return 0;
}

const char *data_bind_value_as_string(const DataBindValue *value) {
  return value != NULL && value->kind == DATA_BIND_VALUE_STRING ? value->data.string_val.ptr : NULL;
}

const uint8_t *data_bind_value_as_bytes(const DataBindValue *value, size_t *len) {
  if (len != NULL) *len = 0;
  if (value == NULL || value->kind != DATA_BIND_VALUE_BYTES) return NULL;
  if (len != NULL) *len = value->data.bytes_val.len;
  return value->data.bytes_val.ptr;
}

int data_bind_value_as_uuid(const DataBindValue *value, uuid_t *out) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_UUID || out == NULL) return 0;
  *out = value->data.uuid_val;
  return 1;
}

const char *data_bind_value_as_uuid_string(const DataBindValue *value, char *out, size_t len) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_UUID || out == NULL ||
      len < (size_t)UUID4_STR_BUFFER_SIZE)
    return NULL;
  return uuid_to_s(value->data.uuid_val, out, (int)len) ? out : NULL;
}

int data_bind_value_as_datetime(const DataBindValue *value, turbo_datetime_t *out) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_DATETIME || out == NULL) return 0;
  *out = value->data.datetime_val;
  return 1;
}

double data_bind_value_as_datetime_timestamp(const DataBindValue *value) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_DATETIME) return -1.0;
  return (double)turbo_datetime_to_time(&value->data.datetime_val);
}

const char *data_bind_value_as_datetime_string(const DataBindValue *value, char *out, size_t len) {
  time_t ts;
  if (value == NULL || value->kind != DATA_BIND_VALUE_DATETIME || out == NULL || len < 32)
    return NULL;
  ts = turbo_datetime_to_time(&value->data.datetime_val);
  if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, out, len) < 0) return NULL;
  return out;
}

int data_bind_value_as_date(const DataBindValue *value, DataBindDate *out) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_DATE || out == NULL) return 0;
  *out = value->data.date_val;
  return 1;
}

const char *data_bind_value_as_date_string(const DataBindValue *value, char *out, size_t len) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_DATE) return NULL;
  return db_date_to_text(value->data.date_val, out, len) ? out : NULL;
}

int data_bind_value_as_time(const DataBindValue *value, DataBindTime *out) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_TIME || out == NULL) return 0;
  *out = value->data.time_val;
  return 1;
}

const char *data_bind_value_as_time_string(const DataBindValue *value, char *out, size_t len) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_TIME) return NULL;
  return db_time_to_text(value->data.time_val, out, len) ? out : NULL;
}

int64_t data_bind_value_as_duration_milliseconds(const DataBindValue *value) {
  return value != NULL && value->kind == DATA_BIND_VALUE_DURATION ? value->data.duration_ms : 0;
}

const char *data_bind_value_as_duration_string(const DataBindValue *value, char *out, size_t len) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_DURATION) return NULL;
  return db_duration_to_text(value->data.duration_ms, out, len) ? out : NULL;
}

int data_bind_value_as_decimal(const DataBindValue *value, DataBindDecimal *out) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_DECIMAL || out == NULL) return 0;
  *out = value->data.decimal_val;
  return 1;
}

const char *data_bind_value_as_decimal_string(const DataBindValue *value, char *out, size_t len) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_DECIMAL) return NULL;
  return db_decimal_to_text(value->data.decimal_val, out, len) ? out : NULL;
}

const char *data_bind_value_as_bigint_string(const DataBindValue *value) {
  return value != NULL && value->kind == DATA_BIND_VALUE_BIGINT ? value->data.bigint_val.ptr : NULL;
}

int data_bind_value_as_money(const DataBindValue *value, DataBindMoney *out) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_MONEY || out == NULL) return 0;
  *out = value->data.money_val;
  return 1;
}

const char *data_bind_value_as_money_string(const DataBindValue *value, char *out, size_t len) {
  if (value == NULL || value->kind != DATA_BIND_VALUE_MONEY) return NULL;
  return db_money_to_text(value->data.money_val, out, len) ? out : NULL;
}

DataBindStatus data_bind_value_get_int32(const DataBindValue *value, int32_t *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind == DATA_BIND_VALUE_INT) {
    *out = value->data.int_val;
    return DATA_BIND_OK;
  }
  if (value->kind == DATA_BIND_VALUE_BOOL) {
    *out = value->data.bool_val ? 1 : 0;
    return DATA_BIND_OK;
  }
  return DATA_BIND_ERR_TYPE_MISMATCH;
}

DataBindStatus data_bind_value_get_int64(const DataBindValue *value, int64_t *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind == DATA_BIND_VALUE_INT64) {
    *out = value->data.int64_val;
    return DATA_BIND_OK;
  }
  if (value->kind == DATA_BIND_VALUE_INT) {
    *out = value->data.int_val;
    return DATA_BIND_OK;
  }
  if (value->kind == DATA_BIND_VALUE_BOOL) {
    *out = value->data.bool_val ? 1 : 0;
    return DATA_BIND_OK;
  }
  return DATA_BIND_ERR_TYPE_MISMATCH;
}

DataBindStatus data_bind_value_get_double(const DataBindValue *value, double *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind == DATA_BIND_VALUE_DOUBLE) {
    *out = value->data.double_val;
    return DATA_BIND_OK;
  }
  if (value->kind == DATA_BIND_VALUE_INT64) {
    *out = (double)value->data.int64_val;
    return DATA_BIND_OK;
  }
  if (value->kind == DATA_BIND_VALUE_INT) {
    *out = (double)value->data.int_val;
    return DATA_BIND_OK;
  }
  if (value->kind == DATA_BIND_VALUE_BOOL) {
    *out = value->data.bool_val ? 1.0 : 0.0;
    return DATA_BIND_OK;
  }
  return DATA_BIND_ERR_TYPE_MISMATCH;
}

DataBindStatus data_bind_value_get_bool(const DataBindValue *value, int *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_BOOL) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.bool_val != 0;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_string(const DataBindValue *value, const char **data, size_t *len) {
  if (data == NULL) return DATA_BIND_ERR_INVALID_ARG;
  *data = NULL;
  if (len != NULL) *len = 0;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_STRING) return DATA_BIND_ERR_TYPE_MISMATCH;
  *data = value->data.string_val.ptr;
  if (len != NULL) *len = value->data.string_val.ptr ? strlen(value->data.string_val.ptr) : 0;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_bytes(const DataBindValue *value, const uint8_t **data, size_t *len) {
  if (data == NULL) return DATA_BIND_ERR_INVALID_ARG;
  *data = NULL;
  if (len != NULL) *len = 0;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_BYTES) return DATA_BIND_ERR_TYPE_MISMATCH;
  *data = value->data.bytes_val.ptr;
  if (len != NULL) *len = value->data.bytes_val.len;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_uuid(const DataBindValue *value, uuid_t *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_UUID) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.uuid_val;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_datetime(const DataBindValue *value, turbo_datetime_t *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_DATETIME) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.datetime_val;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_date(const DataBindValue *value, DataBindDate *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_DATE) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.date_val;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_time(const DataBindValue *value, DataBindTime *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_TIME) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.time_val;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_duration_milliseconds(const DataBindValue *value,
                                                         int64_t *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_DURATION) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.duration_ms;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_decimal(const DataBindValue *value, DataBindDecimal *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_DECIMAL) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.decimal_val;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_bigint(const DataBindValue *value, const char **text,
                                          size_t *len) {
  if (text == NULL) return DATA_BIND_ERR_INVALID_ARG;
  *text = NULL;
  if (len != NULL) *len = 0;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_BIGINT) return DATA_BIND_ERR_TYPE_MISMATCH;
  *text = value->data.bigint_val.ptr;
  if (len != NULL) *len = value->data.bigint_val.ptr != NULL ? strlen(value->data.bigint_val.ptr) : 0;
  return DATA_BIND_OK;
}

DataBindStatus data_bind_value_get_money(const DataBindValue *value, DataBindMoney *out) {
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value == NULL) return DATA_BIND_ERR_INVALID_ARG;
  if (value->kind != DATA_BIND_VALUE_MONEY) return DATA_BIND_ERR_TYPE_MISMATCH;
  *out = value->data.money_val;
  return DATA_BIND_OK;
}

const char *data_bind_schema_kind_name(DataBindSchemaKind kind) {
  switch (kind) {
  case DATA_BIND_SCHEMA_MESSAGE:
    return "message";
  case DATA_BIND_SCHEMA_COMPOSITE:
    return "composite";
  case DATA_BIND_SCHEMA_GROUP:
    return "group";
  case DATA_BIND_SCHEMA_ENUM:
    return "enum";
  case DATA_BIND_SCHEMA_FLAGS:
    return "flags";
  case DATA_BIND_SCHEMA_UNION:
    return "union";
  case DATA_BIND_SCHEMA_SCALAR:
    return "scalar";
  case DATA_BIND_SCHEMA_UNKNOWN:
  default:
    return "unknown";
  }
}

size_t data_bind_schema_type_count(DataBind *codec) {
  size_t count = 0;
  static const char *const lists[] = {"messages", "composites", "groups", "unions", "enums"};
  size_t i;
  if (codec == NULL || codec->schema_root == NULL) return 0;
  for (i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
    Node *list = find_child(codec->schema_root, lists[i]);
    if (list != NULL && list->type == NODE_LIST) count += list->data.list.count;
  }
  return count;
}

int data_bind_schema_type_at(DataBind *codec, size_t index, DataBindSchemaType *out) {
  static const char *const lists[] = {"messages", "composites", "groups", "unions", "enums"};
  size_t i;
  if (codec == NULL || codec->schema_root == NULL || out == NULL) return 0;
  for (i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
    Node *list = find_child(codec->schema_root, lists[i]);
    if (list == NULL || list->type != NODE_LIST) continue;
    if (index < list->data.list.count)
      return fill_schema_type(list->data.list.items[index], lists[i], out);
    index -= list->data.list.count;
  }
  db_reflect_clear(out, out->size, sizeof(*out));
  return 0;
}

int data_bind_schema_find_type(DataBind *codec, const char *name, DataBindSchemaType *out) {
  static const char *const lists[] = {"messages", "composites", "groups", "unions", "enums"};
  size_t i;
  if (codec == NULL || codec->schema_root == NULL || name == NULL || out == NULL) return 0;
  for (i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
    Node *record = find_named_record(codec->schema_root, lists[i], name);
    if (record != NULL) return fill_schema_type(record, lists[i], out);
  }
  db_reflect_clear(out, out->size, sizeof(*out));
  return 0;
}

size_t data_bind_schema_field_count(DataBind *codec, const char *type_name) {
  Node *record;
  Node *fields;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL) return 0;
  record = find_schema_record(codec->schema_root, type_name);
  fields = fields_node_for_record(record);
  return fields != NULL ? fields->data.list.count : 0;
}

int data_bind_schema_field_at(DataBind *codec, const char *type_name, size_t index,
                              DataBindSchemaField *out) {
  Node *record;
  Node *fields;
  if (codec == NULL || codec->schema_root == NULL || type_name == NULL || out == NULL) return 0;
  record = find_schema_record(codec->schema_root, type_name);
  fields = fields_node_for_record(record);
  if (fields == NULL || index >= fields->data.list.count) {
    db_reflect_clear(out, out->size, sizeof(*out));
    return 0;
  }
  return fill_schema_field(codec->schema_root, fields->data.list.items[index], out);
}

size_t data_bind_schema_enum_count(DataBind *codec) {
  Node *enums;
  if (codec == NULL || codec->schema_root == NULL) return 0;
  enums = find_child(codec->schema_root, "enums");
  return enums != NULL && enums->type == NODE_LIST ? enums->data.list.count : 0;
}

int data_bind_schema_enum_at(DataBind *codec, size_t index, DataBindSchemaType *out) {
  Node *enums;
  if (codec == NULL || codec->schema_root == NULL || out == NULL) return 0;
  enums = find_child(codec->schema_root, "enums");
  if (enums == NULL || enums->type != NODE_LIST || index >= enums->data.list.count) {
    db_reflect_clear(out, out->size, sizeof(*out));
    return 0;
  }
  return fill_schema_type(enums->data.list.items[index], "enums", out);
}

size_t data_bind_schema_enum_item_count(DataBind *codec, const char *enum_name) {
  Node *record;
  Node *items;
  if (codec == NULL || codec->schema_root == NULL || enum_name == NULL) return 0;
  record = find_named_record(codec->schema_root, "enums", enum_name);
  items = items_node_for_enum(record);
  return items != NULL ? items->data.list.count : 0;
}

int data_bind_schema_enum_item_at(DataBind *codec, const char *enum_name, size_t index,
                                  DataBindSchemaEnumItem *out) {
  Node *record;
  Node *items;
  Node *item;
  size_t out_size;
  if (codec == NULL || codec->schema_root == NULL || enum_name == NULL || out == NULL) return 0;
  record = find_named_record(codec->schema_root, "enums", enum_name);
  items = items_node_for_enum(record);
  if (items == NULL || index >= items->data.list.count) {
    db_reflect_clear(out, out->size, sizeof(*out));
    return 0;
  }
  item = items->data.list.items[index];
  out_size = db_reflect_out_size(out->size, sizeof(*out));
  memset(out, 0, out_size);
  DB_REFLECT_SET(DataBindSchemaEnumItem, out, out_size, size, out_size);
  DB_REFLECT_SET(DataBindSchemaEnumItem, out, out_size, name,
                 get_string_val(find_child(item, "name")));
  DB_REFLECT_SET(DataBindSchemaEnumItem, out, out_size, value,
                 get_string_val(find_child(item, "value")));
  return get_string_val(find_child(item, "name")) != NULL;
}

const char *data_bind_schema_name(DataBind *codec) {
  Node *schema;
  if (codec == NULL || codec->schema_root == NULL) return NULL;
  schema = find_child(codec->schema_root, "schema");
  return get_string_val(find_child(schema, "name"));
}

size_t data_bind_schema_attribute_count(DataBind *codec) {
  Node *schema;
  Node *attrs;
  if (codec == NULL || codec->schema_root == NULL) return 0;
  schema = find_child(codec->schema_root, "schema");
  attrs = find_child(schema, "attributes");
  return attrs != NULL && attrs->type == NODE_LIST ? attrs->data.list.count : 0;
}

int data_bind_schema_attribute_at(DataBind *codec, size_t index, DataBindSchemaAttribute *out) {
  Node *schema;
  Node *attrs;
  Node *attr;
  size_t out_size;
  if (codec == NULL || codec->schema_root == NULL || out == NULL) return 0;
  schema = find_child(codec->schema_root, "schema");
  attrs = find_child(schema, "attributes");
  if (attrs == NULL || attrs->type != NODE_LIST || index >= attrs->data.list.count) {
    db_reflect_clear(out, out->size, sizeof(*out));
    return 0;
  }
  attr = attrs->data.list.items[index];
  out_size = db_reflect_out_size(out->size, sizeof(*out));
  memset(out, 0, out_size);
  DB_REFLECT_SET(DataBindSchemaAttribute, out, out_size, size, out_size);
  DB_REFLECT_SET(DataBindSchemaAttribute, out, out_size, name,
                 get_string_val(find_child(attr, "name")));
  DB_REFLECT_SET(DataBindSchemaAttribute, out, out_size, value,
                 get_string_val(find_child(attr, "value")));
  return get_string_val(find_child(attr, "name")) != NULL;
}

const char *data_bind_schema_attribute_get(DataBind *codec, const char *name) {
  size_t i;
  size_t count;
  if (codec == NULL || name == NULL) return NULL;
  count = data_bind_schema_attribute_count(codec);
  for (i = 0; i < count; i++) {
    DataBindSchemaAttribute attr = DATA_BIND_SCHEMA_ATTRIBUTE_INIT;
    if (data_bind_schema_attribute_at(codec, i, &attr) && attr.name != NULL &&
        strcmp(attr.name, name) == 0)
      return attr.value;
  }
  return NULL;
}

const char *data_bind_status_name(DataBindStatus status) {
  switch (status) {
  case DATA_BIND_OK: return "ok";
  case DATA_BIND_ERR_INVALID_ARG: return "invalid_arg";
  case DATA_BIND_ERR_IO: return "io";
  case DATA_BIND_ERR_PARSE: return "parse";
  case DATA_BIND_ERR_SCHEMA: return "schema";
  case DATA_BIND_ERR_TYPE_NOT_FOUND: return "type_not_found";
  case DATA_BIND_ERR_TYPE_MISMATCH: return "type_mismatch";
  case DATA_BIND_ERR_OOM: return "oom";
  case DATA_BIND_ERR_RUNTIME: return "runtime";
  default: return "unknown";
  }
}

int data_bind_library_version(void) {
  return DATA_BIND_VERSION;
}

int data_bind_abi_version(void) {
  return DATA_BIND_ABI_VERSION;
}

const char *data_bind_version_string(void) {
  return "1.6.0";
}
