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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct DataBind {
  MIR_context_t ctx;
  Node *schema_root;
  mir_func_node_t *func_head;
  owned_alloc_node_t *owned_allocs;
  char error[256];
  DataBindValueApi api;
};

typedef enum {
  EF_INT,
  EF_I64,
  EF_DBL,
  EF_STR,
  EF_FIX_BYTES,
  EF_VAR_BYTES,
  EF_LIST_INT,
  EF_LIST_I64,
  EF_LIST_DBL,
  EF_LIST_STR,
  EF_LIST_OBJ,
  EF_SET_INT,
  EF_SET_DBL,
  EF_SET_STR,
  EF_MAP_STR_STR,
  EF_MAP_STR_INT,
  EF_MAP_STR_DBL,
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
  external_ref_t create_obj, set_int, set_i64, set_dbl, set_str, set_bytes;
  external_ref_t create_list, add_list_int, add_list_i64, add_list_dbl, add_list_str, add_list_obj,
      set_list;
  external_ref_t create_set, add_set_int, add_set_dbl, add_set_str, set_set;
  external_ref_t create_map, add_map_str_str, add_map_str_int, add_map_str_dbl, set_map;
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

static void set_i64_noop(Value *o, const char *n, int64_t v) {
  (void)o;
  (void)n;
  (void)v;
}
static void set_bytes_noop(Value *o, const char *n, const uint8_t *d, size_t l) {
  (void)o;
  (void)n;
  (void)d;
  (void)l;
}
static void set_i32_noop(Value *o, const char *n, int32_t v) {
  (void)o;
  (void)n;
  (void)v;
}
static void set_dbl_noop(Value *o, const char *n, double v) {
  (void)o;
  (void)n;
  (void)v;
}
static void set_str_noop(Value *o, const char *n, const char *v) {
  (void)o;
  (void)n;
  (void)v;
}
static Value *container_noop(void) { return NULL; }
static void add_i32_noop(Value *v, int32_t x) {
  (void)v;
  (void)x;
}
static void add_i64_noop(Value *v, int64_t x) {
  (void)v;
  (void)x;
}
static void add_dbl_noop(Value *v, double x) {
  (void)v;
  (void)x;
}
static void add_str_noop(Value *v, const char *s) {
  (void)v;
  (void)s;
}
static void add_obj_noop(Value *v, Value *o) {
  (void)v;
  (void)o;
}
static void set_container_noop(Value *o, const char *n, Value *v) {
  (void)o;
  (void)n;
  (void)v;
}
static void add_map_str_str_noop(Value *m, const char *k, const char *v) {
  (void)m;
  (void)k;
  (void)v;
}
static void add_map_str_int_noop(Value *m, const char *k, int32_t v) {
  (void)m;
  (void)k;
  (void)v;
}
static void add_map_str_dbl_noop(Value *m, const char *k, double v) {
  (void)m;
  (void)k;
  (void)v;
}
static Value *create_value_noop(void) { return NULL; }

static const DataBindValueApi MIR_OUTPUT_API = {
    create_value_noop,   set_i32_noop,      set_i64_noop,      set_dbl_noop,
    set_str_noop,        set_bytes_noop,    create_value_noop, add_i32_noop,
    add_i64_noop,        add_dbl_noop,      add_str_noop,      add_obj_noop,
    set_container_noop,  set_container_noop, create_value_noop, add_i32_noop,
    add_dbl_noop,        add_str_noop,      set_container_noop, create_value_noop,
    add_map_str_str_noop, add_map_str_int_noop, add_map_str_dbl_noop, set_container_noop};

static int set_codec_error(DataBind *codec, const char *fmt, ...) {
  va_list ap;
  if (codec == NULL) return 0;
  va_start(ap, fmt);
  vsnprintf(codec->error, sizeof(codec->error), fmt, ap);
  va_end(ap);
  return 0;
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

static int parse_positive_int(const char *text) {
  char *end = NULL;
  long value;
  if (text == NULL) return 0;
  value = strtol(text, &end, 10);
  if (end == text || value <= 0 || value > 0x7fffffffL) return 0;
  return (int)value;
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
    else if (!meta->is_64) kind = EF_SET_INT;
    else return 1;
    return append_emit_field(fields, full_name, kind, meta, meta->size, 0, 0, 0, NULL);
  }

  return append_emit_field(
      fields, full_name, meta->is_float ? EF_LIST_DBL : (meta->is_64 ? EF_LIST_I64 : EF_LIST_INT),
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
                             meta->is_float ? EF_DBL : (meta->is_64 ? EF_I64 : EF_INT), meta,
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
          codec->api.add_map_entry_string_double == NULL))) {
      return set_codec_error(codec, "Schema field '%s.%s' requires container API callbacks",
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
  MIR_var_t set_str_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t set_bytes_args[] = {
      {MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "data", 0}, {MIR_T_I64, "len", 0}};
  MIR_var_t list_i32_args[] = {{MIR_T_P, "list", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t list_i64_args[] = {{MIR_T_P, "list", 0}, {MIR_T_I64, "value", 0}};
  MIR_var_t list_dbl_args[] = {{MIR_T_P, "list", 0}, {MIR_T_D, "value", 0}};
  MIR_var_t list_str_args[] = {{MIR_T_P, "list", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t list_obj_args[] = {{MIR_T_P, "list", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t set_list_args[] = {{MIR_T_P, "obj", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "list", 0}};
  MIR_var_t map_str_str_args[] = {{MIR_T_P, "map", 0}, {MIR_T_P, "key", 0}, {MIR_T_P, "value", 0}};
  MIR_var_t map_str_int_args[] = {
      {MIR_T_P, "map", 0}, {MIR_T_P, "key", 0}, {MIR_T_I32, "value", 0}};
  MIR_var_t map_str_dbl_args[] = {{MIR_T_P, "map", 0}, {MIR_T_P, "key", 0}, {MIR_T_D, "value", 0}};
  MIR_var_t read_varstr_args[] = {{MIR_T_P, "buf", 0}, {MIR_T_I64, "offset", 0}, {MIR_T_I64, "remaining", 0}};
  MIR_var_t free_args[] = {{MIR_T_P, "ptr", 0}};

  declare_external(builder, &builder->ext.create_obj, "create_obj", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.set_int, "set_int", 0, NULL, 3, set_int_args);
  declare_external(builder, &builder->ext.set_i64, "set_int64", 0, NULL, 3, set_i64_args);
  declare_external(builder, &builder->ext.set_dbl, "set_dbl", 0, NULL, 3, set_dbl_args);
  declare_external(builder, &builder->ext.set_str, "set_str", 0, NULL, 3, set_str_args);
  declare_external(builder, &builder->ext.set_bytes, "set_bytes", 0, NULL, 4, set_bytes_args);
  declare_external(builder, &builder->ext.create_list, "create_list", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.add_list_int, "add_list_int", 0, NULL, 2, list_i32_args);
  declare_external(builder, &builder->ext.add_list_i64, "add_list_int64", 0, NULL, 2,
                   list_i64_args);
  declare_external(builder, &builder->ext.add_list_dbl, "add_list_dbl", 0, NULL, 2, list_dbl_args);
  declare_external(builder, &builder->ext.add_list_str, "add_list_str", 0, NULL, 2, list_str_args);
  declare_external(builder, &builder->ext.add_list_obj, "add_list_obj", 0, NULL, 2, list_obj_args);
  declare_external(builder, &builder->ext.set_list, "set_list", 0, NULL, 3, set_list_args);
  declare_external(builder, &builder->ext.create_set, "create_set", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.add_set_int, "add_set_int", 0, NULL, 2, list_i32_args);
  declare_external(builder, &builder->ext.add_set_dbl, "add_set_dbl", 0, NULL, 2, list_dbl_args);
  declare_external(builder, &builder->ext.add_set_str, "add_set_str", 0, NULL, 2, list_str_args);
  declare_external(builder, &builder->ext.set_set, "set_set", 0, NULL, 3, set_list_args);
  declare_external(builder, &builder->ext.create_map, "create_map", 1, &ptr_result, 0, NULL);
  declare_external(builder, &builder->ext.add_map_str_str, "add_map_str_str", 0, NULL, 3,
                   map_str_str_args);
  declare_external(builder, &builder->ext.add_map_str_int, "add_map_str_int", 0, NULL, 3,
                   map_str_int_args);
  declare_external(builder, &builder->ext.add_map_str_dbl, "add_map_str_dbl", 0, NULL, 3,
                   map_str_dbl_args);
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
  if (field->kind == EF_INT || field->kind == EF_I64 || field->kind == EF_DBL) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    args[0] = emitter_reg(e, target_obj_reg);
    args[1] = MIR_new_ref_op(e->builder->ctx, field_name_item);
    if (field->kind == EF_INT) {
      args[2] = emitter_int32_value(e, field, off_reg);
      emitter_call(e, &e->builder->ext.set_int, args, 3);
    } else if (field->kind == EF_I64) {
      args[2] = emitter_mem(e, field->mir_type, off_reg, 0);
      emitter_call(e, &e->builder->ext.set_i64, args, 3);
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
  if (field->kind == EF_LIST_INT || field->kind == EF_LIST_I64 || field->kind == EF_LIST_DBL) {
    emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
    args[0] = emitter_reg(e, list_reg);
    if (field->kind == EF_LIST_INT) {
      args[1] = emitter_int32_value(e, field, off_reg);
      emitter_call(e, &e->builder->ext.add_list_int, args, 2);
    } else if (field->kind == EF_LIST_I64) {
      args[1] = emitter_mem(e, field->mir_type, off_reg, 0);
      emitter_call(e, &e->builder->ext.add_list_i64, args, 2);
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
    } else if (field->kind == EF_MAP_STR_INT || field->kind == EF_MAP_STR_DBL) {
      emitter_append(e, MIR_new_insn(e->builder->ctx, MIR_BEQ, emitter_label(e, skip_label),
                                     emitter_reg(e, key_reg), MIR_new_int_op(e->builder->ctx, 0)));
      args[0] = emitter_reg(e, map_reg);
      args[1] = emitter_reg(e, key_reg);
      if (field->kind == EF_MAP_STR_INT) {
        emitter_bounds_check_const(e, off_reg, field->size, e->fail_label);
        args[2] = emitter_int32_value(e, field, off_reg);
        emitter_call(e, &e->builder->ext.add_map_str_int, args, 3);
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
      field->kind == EF_STR || field->kind == EF_FIX_BYTES || field->kind == EF_VAR_BYTES)
    emit_scalar_field(e, target_obj_reg, off_reg, field, field_name_item);
  else if (field->kind == EF_LIST_INT || field->kind == EF_LIST_I64 || field->kind == EF_LIST_DBL ||
           field->kind == EF_LIST_STR || field->kind == EF_LIST_OBJ)
    emit_list_field(e, target_obj_reg, off_reg, field, field_name_item);
  else if (field->kind == EF_SET_INT || field->kind == EF_SET_DBL || field->kind == EF_SET_STR)
    emit_set_field(e, target_obj_reg, off_reg, field, field_name_item);
  else if (field->kind == EF_MAP_STR_STR || field->kind == EF_MAP_STR_INT ||
           field->kind == EF_MAP_STR_DBL)
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
  MIR_type_t result_type = MIR_T_P;
  MIR_var_t args[] = {{MIR_T_P, "buf", 0}, {MIR_T_I64, "len", 0}};
  char func_name[256];
  mir_emitter_t e;
  if (message_name == NULL) return 0;
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

static Node *load_and_parse_schema(const char *schema_path, char *error_buf, size_t error_size) {
  turbo_fs_buf_t buf;
  Node *root;
  tbe_error_t err = {0};
  if (turbo_fs_read_file(schema_path, &buf) != 0) {
    snprintf(error_buf, error_size, "Cannot read schema: %s", schema_path);
    return NULL;
  }
  root = create_node_map(NULL);
  if (parse_schema(buf.base, buf.len, root, &err) != 0) {
    snprintf(error_buf, error_size, "Parse error: %s", err.message);
    turbo_fs_buf_free(&buf);
    node_free(root);
    return NULL;
  }
  turbo_fs_buf_free(&buf);
  return root;
}

static void data_bind_free_contents(DataBind *codec) {
  mir_func_node_t *func_node;
  owned_alloc_node_t *alloc_node;
  if (codec == NULL) return;
  func_node = codec->func_head;
  while (func_node != NULL) {
    mir_func_node_t *next = func_node->next;
    free(func_node->type_name);
    free(func_node);
    func_node = next;
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
  if (codec->ctx != NULL) {
    MIR_finish(codec->ctx);
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
  codec->ctx = MIR_init();
  if (codec->ctx == NULL) return NULL;
  builder.codec = codec;
  builder.ctx = codec->ctx;
  builder.module = MIR_new_module(codec->ctx, "data_bind_binary");
  builder.temp_name_id = 0;
  builder.data_name_id = 0;
  init_externals(&builder);
  messages_node = find_child(codec->schema_root, "messages");
  if (messages_node == NULL || messages_node->type != NODE_LIST ||
      messages_node->data.list.count == 0) {
    set_codec_error(codec, "No messages found in schema");
    MIR_finish(codec->ctx);
    codec->ctx = NULL;
    return NULL;
  }
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
  MIR_load_external(codec->ctx, "set_str", codec->api.set_field_string);
  MIR_load_external(codec->ctx, "set_bytes",
                    codec->api.set_field_bytes != NULL ? codec->api.set_field_bytes
                                                       : set_bytes_noop);
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

DataBind *data_bind_create(const char *schema_path, const DataBindValueApi *api) {
  DataBind *codec;
  MIR_module_t module;
  if (api == NULL || api->create_object == NULL || api->set_field_int == NULL ||
      api->set_field_double == NULL || api->set_field_string == NULL)
    return NULL;
  codec = (DataBind *)calloc(1, sizeof(*codec));
  if (codec == NULL) return NULL;
  codec->api = *api;
  codec->schema_root = load_and_parse_schema(schema_path, codec->error, sizeof(codec->error));
  if (codec->schema_root == NULL) {
    free(codec);
    return NULL;
  }
  module = generate_parser_module(codec);
  if (module == NULL) {
    data_bind_free(codec);
    return NULL;
  }
  if (!link_module(codec, module)) {
    data_bind_free(codec);
    return NULL;
  }
  register_parse_functions(codec, module);
  return codec;
}

void data_bind_free(DataBind *codec) {
  if (codec == NULL) return;
  data_bind_free_contents(codec);
  free(codec);
}

int data_bind_generate_mir(const char *schema_path, FILE *out, int binary_output, char *error_buf,
                           size_t error_size) {
  DataBind codec;
  MIR_module_t module = NULL;
  int status = 1;

  if (error_buf != NULL && error_size > 0) error_buf[0] = '\0';
  if (schema_path == NULL || out == NULL) {
    if (error_buf != NULL && error_size > 0)
      snprintf(error_buf, error_size, "Invalid MIR output arguments");
    return 1;
  }

  memset(&codec, 0, sizeof(codec));
  codec.api = MIR_OUTPUT_API;
  codec.schema_root = load_and_parse_schema(schema_path, codec.error, sizeof(codec.error));
  if (codec.schema_root == NULL) goto cleanup;

  module = generate_parser_module(&codec);
  if (module == NULL) goto cleanup;

  if (binary_output)
    MIR_write_module(codec.ctx, out, module);
  else
    MIR_output_module(codec.ctx, out, module);
  status = ferror(out) ? 1 : 0;

cleanup:
  if (status != 0 && error_buf != NULL && error_size > 0)
    snprintf(error_buf, error_size, "%s", codec.error[0] != '\0' ? codec.error : "MIR output failed");
  data_bind_free_contents(&codec);
  return status;
}

Value *data_bind_parse(DataBind *codec, const char *type_name, const uint8_t *buf, size_t len) {
  mir_func_node_t *node;
  Value *(*parse_fn)(const uint8_t *, int64_t);
  if (codec == NULL || type_name == NULL || buf == NULL) return NULL;
  for (node = codec->func_head; node != NULL; node = node->next) {
    if (strcmp(node->type_name, type_name) == 0) {
      parse_fn = (Value *(*)(const uint8_t *, int64_t))node->parse_fn;
      codec->error[0] = '\0';
      return parse_fn(buf, (int64_t)len);
    }
  }
  snprintf(codec->error, sizeof(codec->error), "Type not found: %s", type_name);
  return NULL;
}

const char *data_bind_get_error(DataBind *codec) {
  return codec != NULL ? codec->error : "Invalid codec";
}
