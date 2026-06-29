#include "schema_parser_dsl.h"
#include "schema_lexer.h"
#include "schema_types.h"
#include "schema_grammar_gen.h"
#include "tbe_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/*
 * Forward declarations for the lemon-generated parser.
 * The generated code provides these functions:
 *   void *SchemaParseAlloc(void *(*)(size_t));
 *   void  SchemaParseFree(void *, void (*)(void *));
 *   void  SchemaParse(void *, int, schema_token_t, schema_parse_ctx_t *);
 */
void *SchemaParseAlloc(void *(*)(size_t));
void  SchemaParseFree(void *, void (*)(void *));
void  SchemaParse(void *, int, schema_token_t, schema_parse_ctx_t *);

static int is_named_child(const Node *node, const char *name) {
    return node && node->name && strcmp(node->name, name) == 0;
}

static void map_remove_named_children(Node *map, const char *name) {
    size_t out = 0;

    if (!map || map->type != NODE_MAP) {
        return;
    }

    for (size_t i = 0; i < map->data.map.count; ++i) {
        Node *child = map->data.map.items[i];
        if (is_named_child(child, name)) {
            node_free(child);
            continue;
        }
        map->data.map.items[out++] = child;
    }

    map->data.map.count = out;
}

static Node *map_take_named_child(Node *map, const char *name) {
    if (!map || map->type != NODE_MAP) {
        return NULL;
    }

    for (size_t i = 0; i < map->data.map.count; ++i) {
        Node *child = map->data.map.items[i];
        if (!is_named_child(child, name)) {
            continue;
        }

        if (i + 1 < map->data.map.count) {
            memmove(&map->data.map.items[i], &map->data.map.items[i + 1],
                    (map->data.map.count - i - 1) * sizeof(Node *));
        }
        map->data.map.count--;
        return child;
    }

    return NULL;
}

static Node *map_find_named_child(const Node *parent, const char *name) {
    if (!parent || !name) {
        return NULL;
    }

    if (parent->type == NODE_MAP) {
        for (size_t i = 0; i < parent->data.map.count; ++i) {
            Node *child = parent->data.map.items[i];
            if (is_named_child(child, name)) {
                return child;
            }
        }
    } else if (parent->type == NODE_LIST) {
        for (size_t i = 0; i < parent->data.list.count; ++i) {
            Node *child = parent->data.list.items[i];
            if (is_named_child(child, name)) {
                return child;
            }
        }
    }

    return NULL;
}

static const char *map_find_string_value(const Node *map, const char *name) {
    Node *child = map_find_named_child(map, name);

    if (!child || child->type != NODE_STRING) {
        return NULL;
    }

    return child->data.string_val;
}

static int map_has_named_child(const Node *map, const char *name) {
    return map_find_named_child(map, name) != NULL;
}

static int parse_size_text(const char *text, size_t *out) {
    char *end = NULL;
    unsigned long long value;

    if (!text || !out || text[0] == '\0') {
        return 0;
    }

    value = strtoull(text, &end, 10);
    if (!end || *end != '\0') {
        return 0;
    }

    *out = (size_t)value;
    return 1;
}

static int map_set_string(Node *map, const char *name, const char *value) {
    const char *current = map_find_string_value(map, name);
    Node *new_node;

    if (current && strcmp(current, value) == 0) {
        return 0;
    }

    new_node = create_node_string(name, value);
    if (!new_node) {
        return -1;
    }

    map_remove_named_children(map, name);
    if (map_add(map, new_node) != 0) {
        node_free(new_node);
        return -1;
    }
    return 1;
}

static int map_set_size(Node *map, const char *name, size_t value) {
    char buf[32];
    int result;

    snprintf(buf, sizeof(buf), "%zu", value);
    result = map_set_string(map, name, buf);
    return (result < 0) ? -1 : result;
}

static int map_set_true(Node *map, const char *name) {
    int result = map_set_string(map, name, "1");
    return (result < 0) ? -1 : result;
}

static const char *attribute_value(const Node *owner, const char *attr_name) {
    Node *attributes;
    Node *attribute;

    if (!owner || !attr_name) {
        return NULL;
    }

    attributes = map_find_named_child(owner, "attributes");
    if (!attributes) {
        return NULL;
    }

    attribute = map_find_named_child(attributes, attr_name);
    if (!attribute) {
        return NULL;
    }

    return map_find_string_value(attribute, "value");
}

static size_t count_records_in_list(const Node *list) {
    if (!list || list->type != NODE_LIST) {
        return 0;
    }

    return list->data.list.count;
}

typedef struct {
    const char *name;
    size_t size;
    const char *wire_reader;
    const char *host_type;
} type_info_t;

static const type_info_t TYPE_TABLE[] = {
    {"bool",    1, "u8",  "uint8_t"},
    {"uint8_t", 1, "u8",  "uint8_t"},
    {"uint8",   1, "u8",  "uint8_t"},
    {"byte",    1, "u8",  "uint8_t"},
    {"int8_t",  1, "i8",  "int8_t"},
    {"int8",    1, "i8",  "int8_t"},
    {"uint16_t", 2, "u16", "uint16_t"},
    {"uint16",   2, "u16", "uint16_t"},
    {"int16_t",  2, "i16", "int16_t"},
    {"int16",    2, "i16", "int16_t"},
    {"uint32_t", 4, "u32", "uint32_t"},
    {"uint32",   4, "u32", "uint32_t"},
    {"int32_t",  4, "i32", "int32_t"},
    {"int32",    4, "i32", "int32_t"},
    {"float",    4, "f32", "float"},
    {"uint64_t", 8, "u64", "uint64_t"},
    {"uint64",   8, "u64", "uint64_t"},
    {"int64_t",  8, "i64", "int64_t"},
    {"int64",    8, "i64", "int64_t"},
    {"double",   8, "f64", "double"},
};

static const type_info_t *find_type_info(const char *type_name) {
    if (!type_name) return NULL;

    for (size_t i = 0; i < sizeof(TYPE_TABLE) / sizeof(TYPE_TABLE[0]); i++) {
        if (strcmp(type_name, TYPE_TABLE[i].name) == 0) {
            return &TYPE_TABLE[i];
        }
    }
    return NULL;
}

static int primitive_type_size(const char *type_name, size_t *out) {
    const type_info_t *info = find_type_info(type_name);
    if (type_name && strcmp(type_name, "uuid") == 0 && out) {
        *out = 16;
        return 1;
    }
    if (!info || !out) return 0;
    *out = info->size;
    return 1;
}

static int primitive_type_wire_reader(const char *type_name, const char **out) {
    const type_info_t *info = find_type_info(type_name);
    if (!info || !info->wire_reader || !out) return 0;
    *out = info->wire_reader;
    return 1;
}

static int primitive_type_host_type(const char *type_name, const char **out) {
    const type_info_t *info = find_type_info(type_name);
    if (!info || !info->host_type || !out) return 0;
    *out = info->host_type;
    return 1;
}

static Node *find_composite_by_name(const Node *root, const char *name) {
    static const char *record_lists[] = { "composites" };

    if (!root || !name) {
        return NULL;
    }

    for (size_t list_index = 0; list_index < sizeof(record_lists) / sizeof(record_lists[0]);
         ++list_index) {
        Node *list = map_find_named_child(root, record_lists[list_index]);
        if (!list || list->type != NODE_LIST) {
            continue;
        }

        for (size_t i = 0; i < list->data.list.count; ++i) {
            Node *record = list->data.list.items[i];
            const char *record_name = map_find_string_value(record, "name");

            if (record_name && strcmp(record_name, name) == 0) {
                return record;
            }
        }
    }

    return NULL;
}

static Node *find_enum_by_name(const Node *root, const char *name) {
    Node *enums;

    if (!root || !name) {
        return NULL;
    }

    enums = map_find_named_child(root, "enums");
    if (!enums || enums->type != NODE_LIST) {
        return NULL;
    }

    for (size_t i = 0; i < enums->data.list.count; ++i) {
        Node *enum_node = enums->data.list.items[i];
        const char *enum_name = map_find_string_value(enum_node, "name");

        if (enum_name && strcmp(enum_name, name) == 0) {
            return enum_node;
        }
    }

    return NULL;
}

static int resolve_enum_fixed_size(const Node *root, const char *type_name, size_t *out) {
    Node *enum_node;
    const char *underlying_type;

    if (!root || !type_name || !out) {
        return 0;
    }

    enum_node = find_enum_by_name(root, type_name);
    if (!enum_node) {
        return 0;
    }

    underlying_type = map_find_string_value(enum_node, "underlying_type");
    if (!underlying_type || underlying_type[0] == '\0') {
        underlying_type = "int32";
    }

    return primitive_type_size(underlying_type, out);
}

static int resolve_type_fixed_size(const Node *root, const char *type_name, size_t *out) {
    Node *record;

    if (primitive_type_size(type_name, out)) {
        return 1;
    }

    record = find_composite_by_name(root, type_name);
    if (!record || !map_has_named_child(record, "has_fixed_block_size")) {
        return resolve_enum_fixed_size(root, type_name, out);
    }

    return parse_size_text(map_find_string_value(record, "fixed_block_size"), out);
}

static int resolve_field_fixed_size(const Node *root, const Node *field, size_t *out) {
    const char *type_name;
    const char *length_field;
    const char *inner_type;
    size_t inner_size = 0;
    size_t element_count = 0;

    if (!field || !out) {
        return 0;
    }

    if (map_has_named_child(field, "is_group_field") || map_has_named_child(field, "is_var_data")) {
        return 0;
    }

    if (map_has_named_child(field, "is_numeric") || map_has_named_child(field, "is_bytes")) {
        return parse_size_text(map_find_string_value(field, "size_bytes"), out);
    }

    if (map_has_named_child(field, "is_collection")) {
        length_field = map_find_string_value(field, "length_field");
        inner_type = map_find_string_value(field, "inner_type");

        if (!map_has_named_child(field, "is_fixed_size") ||
            !parse_size_text(length_field, &element_count) ||
            !resolve_type_fixed_size(root, inner_type, &inner_size)) {
            return 0;
        }

        // Check for overflow before multiplication
        if (element_count > 0 && inner_size > SIZE_MAX / element_count) {
            return 0;  // Overflow would occur
        }

        *out = inner_size * element_count;
        return 1;
    }

    type_name = map_find_string_value(field, "type");
    return resolve_type_fixed_size(root, type_name, out);
}

static int annotate_record_layout(const Node *root, Node *record) {
    Node *fields = map_find_named_child(record, "fields");
    size_t offset = 0;
    int unresolved_prefix = 0;
    int changed = 0;
    int result;

    if (!fields || fields->type != NODE_LIST) {
        return 0;
    }

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        size_t field_size = 0;

        if (map_has_named_child(field, "is_group_field") || map_has_named_child(field, "is_var_data")) {
            break;
        }

        if (!resolve_field_fixed_size(root, field, &field_size)) {
            unresolved_prefix = 1;
            break;
        }

        // Check for overflow before adding field_size to offset
        if (field_size > SIZE_MAX - offset) {
            unresolved_prefix = 1;  // Treat overflow as unresolved
            break;
        }

        result = map_set_size(field, "offset", offset);
        changed |= (result > 0);
        result = map_set_true(field, "has_offset");
        changed |= (result > 0);
        result = map_set_size(field, "field_size_bytes", field_size);
        changed |= (result > 0);
        offset += field_size;
    }

    if (!unresolved_prefix) {
        result = map_set_size(record, "fixed_block_size", offset);
        changed |= (result > 0);
        result = map_set_true(record, "has_fixed_block_size");
        changed |= (result > 0);
    }

    return changed;
}

static void annotate_wire_constants(Node *root) {
    static const char *record_lists[] = { "composites", "groups", "messages" };
    Node *schema = map_find_named_child(root, "schema");
    const char *schema_name = schema ? map_find_string_value(schema, "schema_name") : NULL;
    char wire_name[256];

    if (schema_name && schema_name[0]) {
        snprintf(wire_name, sizeof(wire_name), "%s_WIRE_BIG_ENDIAN", schema_name);
    } else {
        snprintf(wire_name, sizeof(wire_name), "GeneratedSchema_WIRE_BIG_ENDIAN");
    }

    for (size_t list_index = 0; list_index < sizeof(record_lists) / sizeof(record_lists[0]);
         ++list_index) {
        Node *list = map_find_named_child(root, record_lists[list_index]);
        if (!list || list->type != NODE_LIST) {
            continue;
        }

        for (size_t i = 0; i < list->data.list.count; ++i) {
            Node *record = list->data.list.items[i];
            Node *fields = map_find_named_child(record, "fields");

            map_set_string(record, "wire_endian_const", wire_name);
            if (!fields || fields->type != NODE_LIST) {
                continue;
            }

            for (size_t field_index = 0; field_index < fields->data.list.count; ++field_index) {
                map_set_string(fields->data.list.items[field_index], "wire_endian_const", wire_name);
            }
        }
    }
}

static Node *find_group_by_name(const Node *root, const char *name) {
    Node *groups = map_find_named_child(root, "groups");

    if (!groups || groups->type != NODE_LIST || !name) {
        return NULL;
    }

    for (size_t i = 0; i < groups->data.list.count; ++i) {
        Node *group = groups->data.list.items[i];
        const char *group_name = map_find_string_value(group, "name");

        if (group_name && strcmp(group_name, name) == 0) {
            return group;
        }
    }

    return NULL;
}

static int record_has_only_fixed_fields(const Node *record) {
    Node *fields = map_find_named_child(record, "fields");

    if (!fields || fields->type != NODE_LIST) {
        return 0;
    }

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];

        if (map_has_named_child(field, "is_group_field") || map_has_named_child(field, "is_var_data")) {
            return 0;
        }
    }

    return 1;
}

static void annotate_group_cursors(Node *root) {
    static const char *record_lists[] = { "composites", "groups", "messages" };
    Node *groups = map_find_named_child(root, "groups");

    if (groups && groups->type == NODE_LIST) {
        for (size_t i = 0; i < groups->data.list.count; ++i) {
            Node *group = groups->data.list.items[i];

            if (!map_has_named_child(group, "has_fixed_block_size") ||
                !record_has_only_fixed_fields(group)) {
                continue;
            }

            map_set_true(group, "supports_group_cursor");
            map_set_size(group, "group_dimension_size", 4);
        }
    }

    for (size_t list_index = 0; list_index < sizeof(record_lists) / sizeof(record_lists[0]);
         ++list_index) {
        Node *list = map_find_named_child(root, record_lists[list_index]);
        const char *previous_group_name = NULL;
        const char *previous_group_type = NULL;
        int previous_group_supported = 1;

        if (!list || list->type != NODE_LIST) {
            continue;
        }

        for (size_t record_index = 0; record_index < list->data.list.count; ++record_index) {
            Node *record = list->data.list.items[record_index];
            Node *fields = map_find_named_child(record, "fields");

            previous_group_name = NULL;
            previous_group_type = NULL;
            previous_group_supported = 1;

            if (!fields || fields->type != NODE_LIST) {
                continue;
            }

            for (size_t field_index = 0; field_index < fields->data.list.count; ++field_index) {
                Node *field = fields->data.list.items[field_index];
                Node *group_record;
                const char *group_type;
                int supported;

                if (!map_has_named_child(field, "is_group_field")) {
                    continue;
                }

                group_type = map_find_string_value(field, "group_type");
                group_record = find_group_by_name(root, group_type);
                supported = group_record && map_has_named_child(group_record, "supports_group_cursor");

                if (supported) {
                    map_set_true(field, "supports_group_cursor");
                    map_set_size(field, "group_dimension_size", 4);
                }

                if (!previous_group_name) {
                    map_set_true(field, "is_first_group_field");
                } else {
                    map_set_string(field, "previous_group_field_name", previous_group_name);
                    if (previous_group_type) {
                        map_set_string(field, "previous_group_type", previous_group_type);
                    }
                }

                if (supported && previous_group_supported) {
                    map_set_true(field, "group_cursor_accessible");
                }

                previous_group_name = map_find_string_value(field, "name");
                previous_group_type = group_type;
                previous_group_supported = supported && previous_group_supported;
            }
        }
    }
}

static void annotate_var_data_accessors(Node *root) {
    static const char *record_lists[] = { "groups", "messages" };

    for (size_t list_index = 0; list_index < sizeof(record_lists) / sizeof(record_lists[0]);
         ++list_index) {
        Node *list = map_find_named_child(root, record_lists[list_index]);

        if (!list || list->type != NODE_LIST) {
            continue;
        }

        for (size_t record_index = 0; record_index < list->data.list.count; ++record_index) {
            Node *record = list->data.list.items[record_index];
            Node *fields = map_find_named_child(record, "fields");
            const char *previous_group_name = NULL;
            const char *previous_group_type = NULL;
            const char *previous_var_data_name = NULL;
            int previous_group_accessible = 1;
            int previous_var_data_accessible = 1;

            if (!fields || fields->type != NODE_LIST) {
                continue;
            }

            for (size_t field_index = 0; field_index < fields->data.list.count; ++field_index) {
                Node *field = fields->data.list.items[field_index];
                const char *field_name;
                int accessible = 0;

                if (map_has_named_child(field, "is_group_field")) {
                    previous_group_name = map_find_string_value(field, "name");
                    previous_group_type = map_find_string_value(field, "group_type");
                    previous_group_accessible = map_has_named_child(field, "group_cursor_accessible");
                    continue;
                }

                if (!map_has_named_child(field, "is_var_data")) {
                    continue;
                }

                if (!previous_var_data_name && map_has_named_child(record, "has_fixed_block_size")) {
                    map_set_true(field, "is_first_var_data_field");
                }

                if (previous_var_data_name) {
                    if (previous_var_data_accessible) {
                        map_set_true(field, "var_data_from_previous_var_data");
                        map_set_string(field, "previous_var_data_field_name", previous_var_data_name);
                        accessible = 1;
                    }
                } else if (previous_group_name) {
                    if (previous_group_accessible && previous_group_type) {
                        map_set_true(field, "var_data_from_previous_group");
                        map_set_string(field, "previous_group_field_name", previous_group_name);
                        map_set_string(field, "previous_group_type", previous_group_type);
                        accessible = 1;
                    }
                } else {
                    map_set_true(field, "var_data_from_block_length");
                    accessible = map_has_named_child(record, "has_fixed_block_size");
                }

                if (accessible) {
                    map_set_true(field, "var_data_accessor_accessible");
                }

                field_name = map_find_string_value(field, "name");
                previous_var_data_name = field_name;
                previous_var_data_accessible = accessible;
            }
        }
    }
}

static void annotate_optional_fields(Node *root) {
    static const char *record_lists[] = { "messages", "composites", "groups" };
    
    for (size_t list_idx = 0; list_idx < sizeof(record_lists) / sizeof(record_lists[0]); ++list_idx) {
        Node *list = map_find_named_child(root, record_lists[list_idx]);
        if (!list || list->type != NODE_LIST) {
            continue;
        }

        for (size_t i = 0; i < list->data.list.count; ++i) {
            Node *record = list->data.list.items[i];
            Node *fields = map_find_named_child(record, "fields");
            
            if (!fields || fields->type != NODE_LIST) {
                continue;
            }

            // 统计可选字段
            size_t optional_count = 0;
            size_t default_count = 0;
            
            for (size_t j = 0; j < fields->data.list.count; ++j) {
                Node *field = fields->data.list.items[j];
                if (map_find_named_child(field, "is_optional")) {
                    optional_count++;
                }
                if (map_find_named_child(field, "has_default")) {
                    default_count++;
                }
            }

            if (optional_count > 0) {
                // 添加has_optional_fields标记
                map_add(record, create_node_string("has_optional_fields", "1"));
                
                // 添加可选字段数量
                char count_str[32];
                snprintf(count_str, sizeof(count_str), "%zu", optional_count);
                map_add(record, create_node_string("optional_field_count", count_str));
                
                // 计算位图大小（按字节）
                size_t bitmap_bytes = (optional_count + 7) / 8;
                char bitmap_size_str[32];
                snprintf(bitmap_size_str, sizeof(bitmap_size_str), "%zu", bitmap_bytes);
                map_add(record, create_node_string("presence_bitmap_bytes", bitmap_size_str));

                // 调整固定块大小以包含位图
                const char *original_block_size_str = map_find_string_value(record, "fixed_block_size");
                if (original_block_size_str) {
                    size_t original_size = (size_t)strtoul(original_block_size_str, NULL, 10);
                    size_t new_size = original_size + bitmap_bytes;
                    char new_size_str[32];
                    snprintf(new_size_str, sizeof(new_size_str), "%zu", new_size);
                    
                    // 更新固定块大小
                    map_remove_named_children(record, "fixed_block_size");
                    map_add(record, create_node_string("fixed_block_size", new_size_str));
                    
                    // 为所有有偏移的字段调整偏移量
                    for (size_t j = 0; j < fields->data.list.count; ++j) {
                        Node *field = fields->data.list.items[j];
                        const char *offset_str = map_find_string_value(field, "offset");
                        if (offset_str) {
                            size_t original_offset = (size_t)strtoul(offset_str, NULL, 10);
                            size_t new_offset = original_offset + bitmap_bytes;
                            char new_offset_str[32];
                            snprintf(new_offset_str, sizeof(new_offset_str), "%zu", new_offset);
                            
                            // 更新字段偏移
                            map_remove_named_children(field, "offset");
                            map_add(field, create_node_string("offset", new_offset_str));
                        }
                    }
                }

                // 创建可选字段列表
                Node *optional_fields_list = create_node_list("optional_fields");
                Node *default_fields_list = create_node_list("default_value_fields");
                
                size_t optional_index = 0;
                for (size_t j = 0; j < fields->data.list.count; ++j) {
                    Node *field = fields->data.list.items[j];
                    
                    if (map_find_named_child(field, "is_optional")) {
                        // 创建可选字段条目
                        Node *optional_field = create_node_map(NULL);
                        const char *field_name = map_find_string_value(field, "name");
                        const char *owner_name = map_find_string_value(field, "owner_name");
                        
                        map_add(optional_field, create_node_string("name", field_name ? field_name : ""));
                        map_add(optional_field, create_node_string("owner_name", owner_name ? owner_name : ""));
                        
                        char bit_index_str[32];
                        snprintf(bit_index_str, sizeof(bit_index_str), "%zu", optional_index);
                        map_add(optional_field, create_node_string("optional_bit_index", bit_index_str));
                        
                        // 添加last标记
                        if (optional_index == optional_count - 1) {
                            map_add(optional_field, create_node_string("last", "1"));
                        }
                        
                        list_add(optional_fields_list, optional_field);
                        optional_index++;
                    }
                    
                    if (map_find_named_child(field, "has_default")) {
                        // 创建默认值字段条目（复制字段信息）
                        Node *default_field = create_node_map(NULL);
                        
                        // 复制所有字段属性
                        for (size_t k = 0; k < field->data.map.count; ++k) {
                            Node *attr = field->data.map.items[k];
                            if (attr->name) {
                                const char *value = attr->type == NODE_STRING ? attr->data.string_val : "";
                                map_add(default_field, create_node_string(attr->name, value));
                            }
                        }
                        
                        // 添加类型推断
                        const char *default_value = map_find_string_value(field, "default_value");
                        const char *field_type = map_find_string_value(field, "type");
                        
                        if (default_value && field_type) {
                            if (strcmp(field_type, "string") == 0) {
                                map_add(default_field, create_node_string("is_string", "1"));
                            } else if (strstr(field_type, "uint") || strstr(field_type, "int") || 
                                     strstr(field_type, "float") || strstr(field_type, "double")) {
                                map_add(default_field, create_node_string("is_numeric", "1"));
                            } else if (strcmp(default_value, "true") == 0 || strcmp(default_value, "false") == 0) {
                                map_add(default_field, create_node_string("is_boolean", "1"));
                            }
                            
                            // 检查是否是枚举引用
                            Node *enums = map_find_named_child(root, "enums");
                            if (enums && enums->type == NODE_LIST) {
                                for (size_t e = 0; e < enums->data.list.count; ++e) {
                                    Node *enum_node = enums->data.list.items[e];
                                    const char *enum_name = map_find_string_value(enum_node, "enum_name");
                                    if (enum_name && strcmp(field_type, enum_name) == 0) {
                                        map_add(default_field, create_node_string("is_enum_ref", "1"));
                                        map_add(default_field, create_node_string("enum_name", enum_name));
                                        break;
                                    }
                                }
                            }
                        }
                        
                        list_add(default_fields_list, default_field);
                    }
                }
                
                map_add(record, optional_fields_list);
                map_add(record, default_fields_list);
            }
        }
    }
}

static void annotate_enum_helpers(Node *root) {
    Node *enums = map_find_named_child(root, "enums");
    if (!enums || enums->type != NODE_LIST) {
        return;
    }

    for (size_t i = 0; i < enums->data.list.count; ++i) {
        Node *enum_node = enums->data.list.items[i];
        Node *items = map_find_named_child(enum_node, "items");
        
        if (!items || items->type != NODE_LIST) {
            continue;
        }

        // 计算项目数量
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%zu", items->data.list.count);
        map_add(enum_node, create_node_string("items_count", count_str));

        // 找出最小值和最大值
        int min_val = INT_MAX;
        int max_val = INT_MIN;
        
        for (size_t j = 0; j < items->data.list.count; ++j) {
            Node *item = items->data.list.items[j];
            const char *value_str = map_find_string_value(item, "value");
            
            if (value_str) {
                int val = (int)strtol(value_str, NULL, 0);
                if (val < min_val) min_val = val;
                if (val > max_val) max_val = val;
            }
        }

        // 添加最小值和最大值
        if (min_val != INT_MAX && max_val != INT_MIN) {
            const char *enum_name = map_find_string_value(enum_node, "enum_name");
            
            // 找到对应的枚举项名称
            const char *min_name = NULL;
            const char *max_name = NULL;
            
            for (size_t j = 0; j < items->data.list.count; ++j) {
                Node *item = items->data.list.items[j];
                const char *value_str = map_find_string_value(item, "value");
                const char *name_str = map_find_string_value(item, "name");
                
                if (value_str && name_str) {
                    int val = (int)strtol(value_str, NULL, 0);
                    if (val == min_val) min_name = name_str;
                    if (val == max_val) max_name = name_str;
                }
            }
            
            if (min_name && max_name && enum_name) {
                char min_value[128];
                char max_value[128];
                snprintf(min_value, sizeof(min_value), "%s_%s", enum_name, min_name);
                snprintf(max_value, sizeof(max_value), "%s_%s", enum_name, max_name);
                
                map_add(enum_node, create_node_string("min_value", min_value));
                map_add(enum_node, create_node_string("max_value", max_value));
            }
        }
    }
}

static void annotate_type_references(Node *root) {
    static const char *record_lists[] = { "composites", "groups", "messages" };

    for (size_t list_index = 0; list_index < sizeof(record_lists) / sizeof(record_lists[0]);
         ++list_index) {
        Node *list = map_find_named_child(root, record_lists[list_index]);

        if (!list || list->type != NODE_LIST) {
            continue;
        }

        for (size_t record_index = 0; record_index < list->data.list.count; ++record_index) {
            Node *record = list->data.list.items[record_index];
            Node *fields = map_find_named_child(record, "fields");

            if (!fields || fields->type != NODE_LIST) {
                continue;
            }

            for (size_t field_index = 0; field_index < fields->data.list.count; ++field_index) {
                Node *field = fields->data.list.items[field_index];
                const char *type_name;

                if (map_has_named_child(field, "is_collection") &&
                    map_has_named_child(field, "is_fixed_size")) {
                    const char *inner_type = map_find_string_value(field, "inner_type");
                    size_t element_size = 0;

                    if (inner_type && resolve_type_fixed_size(root, inner_type, &element_size)) {
                        map_set_size(field, "element_size_bytes", element_size);
                    }

                    if (inner_type && find_composite_by_name(root, inner_type)) {
                        map_set_true(field, "collection_element_is_composite");
                        continue;
                    }

                    if (inner_type && find_enum_by_name(root, inner_type)) {
                        Node *enum_node = find_enum_by_name(root, inner_type);
                        const char *underlying_type = map_find_string_value(enum_node, "underlying_type");
                        const char *host_type = NULL;
                        const char *wire_reader = NULL;
                        char enum_c_type[256];

                        if (!underlying_type || underlying_type[0] == '\0') {
                            underlying_type = "int32";
                        }

                        if (!primitive_type_wire_reader(underlying_type, &wire_reader) ||
                            !primitive_type_host_type(underlying_type, &host_type)) {
                            continue;
                        }

                        snprintf(enum_c_type, sizeof(enum_c_type), "%s_t", inner_type);
                        map_set_true(field, "collection_element_is_enum");
                        map_set_string(field, "collection_element_enum_c_type", enum_c_type);
                        map_set_string(field, "collection_element_host_type", host_type);
                        map_set_string(field, "collection_element_wire_reader", wire_reader);
                        continue;
                    }

                    if (inner_type) {
                        const char *host_type = NULL;
                        const char *wire_reader = NULL;

                        if (primitive_type_wire_reader(inner_type, &wire_reader) &&
                            primitive_type_host_type(inner_type, &host_type)) {
                            map_set_true(field, "collection_element_is_primitive");
                            map_set_string(field, "collection_element_host_type", host_type);
                            map_set_string(field, "collection_element_wire_reader", wire_reader);
                        }
                    }

                    continue;
                }

                if (!map_has_named_child(field, "is_user_defined")) {
                    continue;
                }

                type_name = map_find_string_value(field, "type");
                if (!type_name) {
                    continue;
                }

                if (find_composite_by_name(root, type_name)) {
                    map_set_true(field, "is_composite_ref");
                    map_remove_named_children(field, "is_enum_ref");
                    map_remove_named_children(field, "enum_c_type");
                    map_remove_named_children(field, "enum_host_type");
                    map_remove_named_children(field, "enum_wire_reader");
                    continue;
                }

                if (find_enum_by_name(root, type_name)) {
                    Node *enum_node = find_enum_by_name(root, type_name);
                    const char *underlying_type = map_find_string_value(enum_node, "underlying_type");
                    const char *host_type = NULL;
                    const char *wire_reader = NULL;
                    char enum_c_type[256];

                    if (!underlying_type || underlying_type[0] == '\0') {
                        underlying_type = "int32";
                    }

                    if (!primitive_type_wire_reader(underlying_type, &wire_reader) ||
                        !primitive_type_host_type(underlying_type, &host_type)) {
                        continue;
                    }

                    snprintf(enum_c_type, sizeof(enum_c_type), "%s_t", type_name);
                    map_set_true(field, "is_enum_ref");
                    map_remove_named_children(field, "is_composite_ref");
                    map_set_string(field, "enum_c_type", enum_c_type);
                    map_set_string(field, "enum_host_type", host_type);
                    map_set_string(field, "enum_wire_reader", wire_reader);
                }
            }
        }
    }
}

static void annotate_layouts(Node *root) {
    static const char *record_lists[] = { "composites", "groups", "messages" };

    while (1) {
        int changed = 0;

        for (size_t list_index = 0; list_index < sizeof(record_lists) / sizeof(record_lists[0]);
             ++list_index) {
            Node *list = map_find_named_child(root, record_lists[list_index]);
            if (!list || list->type != NODE_LIST) {
                continue;
            }

            for (size_t i = 0; i < list->data.list.count; ++i) {
                changed |= annotate_record_layout(root, list->data.list.items[i]);
            }
        }

        if (!changed) {
            break;
        }
    }
}

static void annotate_schema_metadata(Node *root) {
    Node *schema = map_find_named_child(root, "schema");
    Node *attributes;
    const char *byte_order;
    char attrs_buf[512];
    size_t attrs_len = 0;
    const char *wire_value = "0";

    if (!schema) {
        return;
    }

    attributes = map_find_named_child(schema, "attributes");
    attrs_buf[0] = '\0';
    if (attributes && attributes->type == NODE_MAP) {
        for (size_t i = 0; i < attributes->data.map.count; ++i) {
            Node *attribute = attributes->data.map.items[i];
            const char *attr_name;
            const char *attr_value;
            int written;

            if (!attribute || attribute->type != NODE_MAP) {
                continue;
            }

            attr_name = map_find_string_value(attribute, "name");
            attr_value = map_find_string_value(attribute, "value");
            if (!attr_name || !attr_value) {
                continue;
            }

            written = snprintf(attrs_buf + attrs_len, sizeof(attrs_buf) - attrs_len,
                               "[ %s: %s ] ", attr_name, attr_value);
            if (written < 0 || (size_t)written >= sizeof(attrs_buf) - attrs_len) {
                break;
            }
            attrs_len += (size_t)written;
        }
    }
    map_set_string(schema, "schema_attributes_rendered", attrs_buf);

    byte_order = attribute_value(schema, "byte_order");
    if (byte_order && strcmp(byte_order, "big") == 0) {
        map_set_string(schema, "wire_byte_order", "big");
        map_set_true(schema, "is_big_endian");
        map_remove_named_children(schema, "is_little_endian");
        wire_value = "1";
    } else {
        map_set_string(schema, "wire_byte_order", "little");
        map_set_true(schema, "is_little_endian");
        map_remove_named_children(schema, "is_big_endian");
    }
    map_set_string(schema, "schema_wire_big_endian_value", wire_value);

    /* Extract version attribute and expose as schema_version */
    {
        const char *ver = attribute_value(schema, "version");
        if (ver && ver[0]) {
            map_set_string(schema, "schema_version", ver);
            map_set_true(schema, "has_schema_version");
        }
    }
}

static Node *parse_schema_raw(const char *text, size_t len, tbe_error_t *err) {
    Node *temp_root;
    Node *schema_node;
    Node *messages_list;
    Node *composites_list;
    Node *groups_list;
    Node *enums_list;

    temp_root = create_node_map(NULL);
    schema_node = NULL;
    messages_list = create_node_list("messages");
    composites_list = create_node_list("composites");
    groups_list = create_node_list("groups");
    enums_list = create_node_list("enums");
    Node *unions_list = create_node_list("unions");
    if (!temp_root || !messages_list || !composites_list || !groups_list || !enums_list || !unions_list) {
        node_free(unions_list);
        node_free(enums_list);
        node_free(groups_list);
        node_free(composites_list);
        node_free(messages_list);
        node_free(temp_root);
        if (err) {
            tbe_error_set(err, TBE_ERR_OUT_OF_MEMORY, -1, -1, "Failed to allocate node structures");
        }
        return NULL;
    }

    if (map_add(temp_root, messages_list) != 0 ||
        map_add(temp_root, composites_list) != 0 ||
        map_add(temp_root, groups_list) != 0 ||
        map_add(temp_root, enums_list) != 0 ||
        map_add(temp_root, unions_list) != 0) {
        node_free(temp_root);
        if (err) {
            tbe_error_set(err, TBE_ERR_OUT_OF_MEMORY, -1, -1, "Failed to add child nodes");
        }
        return NULL;
    }

    schema_parse_ctx_t ctx = {0};
    ctx.root         = temp_root;
    ctx.schema_node  = schema_node;
    ctx.messages_list = messages_list;
    ctx.composites_list = composites_list;
    ctx.groups_list = groups_list;
    ctx.enums_list   = enums_list;
    ctx.unions_list  = unions_list;
    ctx.cur_record   = NULL;
    ctx.cur_fields   = NULL;
    ctx.cur_enum     = NULL;
    ctx.cur_enum_items = NULL;
    ctx.cur_record_kind = SCHEMA_RECORD_COMPOSITE;
    ctx.cur_field_section = SCHEMA_FIELD_SECTION_FIXED;
    ctx.next_enum_value = 0;
    ctx.error        = 0;

    schema_lexer_t lexer;
    schema_lexer_init(&lexer, text, len);

    void *parser = SchemaParseAlloc(malloc);
    if (!parser) {
        node_free(temp_root);
        if (err) {
            tbe_error_set(err, TBE_ERR_OUT_OF_MEMORY, -1, -1, "Failed to allocate parser");
        }
        return NULL;
    }

    schema_token_t tok;
    int rc;
    while ((rc = schema_lexer_next(&lexer, &tok)) > 0) {
        SchemaParse(parser, tok.type, tok, &ctx);
        if (ctx.error) break;
    }

    if (rc < 0) {
        if (err) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Lexer error at line %d", lexer.line);
            tbe_error_set(err, TBE_ERR_LEXER_ERROR, lexer.line, -1, msg);
        }
        ctx.error = 1;
    }

    if (!ctx.error) {
        schema_token_t eof_tok = {0};
        SchemaParse(parser, 0, eof_tok, &ctx);
    }

    SchemaParseFree(parser, free);
    if (ctx.error) {
        node_free(temp_root);
        if (err && err->code == TBE_OK) {
            tbe_error_set(err, TBE_ERR_SYNTAX_ERROR, -1, -1, "Parse error");
        }
        return NULL;
    }

    return temp_root;
}

static void annotate_unions(Node *root) {
    Node *unions = map_find_named_child(root, "unions");
    if (!unions || unions->type != NODE_LIST) return;

    for (size_t i = 0; i < unions->data.list.count; ++i) {
        Node *u = unions->data.list.items[i];
        const char *union_name = map_find_string_value(u, "name");
        Node *fields = map_find_named_child(u, "fields");
        if (!fields || fields->type != NODE_LIST) continue;

        for (size_t j = 0; j < fields->data.list.count; ++j) {
            Node *field = fields->data.list.items[j];
            map_set_size(field, "field_index", j);
            if (union_name) {
                map_set_string(field, "owner_name", union_name);
            }
        }
    }
}

static void annotate_schema_tree(Node *root) {
    annotate_schema_metadata(root);
    annotate_wire_constants(root);
    annotate_layouts(root);
    annotate_type_references(root);
    annotate_group_cursors(root);
    annotate_var_data_accessors(root);
    annotate_optional_fields(root);
    annotate_enum_helpers(root);
    annotate_unions(root);
}

static int merge_schema_into_root(Node *root, Node *parsed) {
    Node *generated_children[6];
    const char *generated_names[] = {
        "schema", "messages", "composites", "groups", "enums", "unions"
    };

    for (size_t i = 0; i < sizeof(generated_children) / sizeof(generated_children[0]); ++i) {
        generated_children[i] = map_take_named_child(parsed, generated_names[i]);
        if (i > 0 && !generated_children[i]) {
            for (size_t j = 0; j < i; ++j) {
                node_free(generated_children[j]);
            }
            return -1;
        }
    }

    for (size_t i = 0; i < sizeof(generated_children) / sizeof(generated_children[0]); ++i) {
        map_remove_named_children(root, generated_names[i]);
        if (generated_children[i]) {
            if (map_add(root, generated_children[i]) != 0) {
                node_free(generated_children[i]);
                for (size_t j = i + 1; j < sizeof(generated_children) / sizeof(generated_children[0]); ++j) {
                    node_free(generated_children[j]);
                }
                return -1;
            }
        }
    }
    return 0;
}

int parse_schema(const char *text, size_t len, Node *root, tbe_error_t *err) {
    if (err) {
        tbe_error_init(err);
    }

    if (!text || !root || root->type != NODE_MAP) {
        if (err) {
            tbe_error_set(err, TBE_ERR_INVALID_ARGUMENT, -1, -1, "Invalid arguments to parse_schema");
        }
        return -1;
    }
    
    // Check for unreasonably large input (prevent DoS)
    if (len > 10 * 1024 * 1024) {  // 10MB limit
        if (err) {
            tbe_error_set(err, TBE_ERR_INVALID_ARGUMENT, -1, -1, "Schema text too large");
        }
        return -1;
    }

    Node *parsed = parse_schema_raw(text, len, err);
    if (!parsed) {
        return -1;
    }

    annotate_schema_tree(parsed);

    int result = merge_schema_into_root(root, parsed);
    node_free(parsed);

    if (result != 0 && err && err->code == TBE_OK) {
        tbe_error_set(err, TBE_ERR_OUT_OF_MEMORY, -1, -1, "Failed to merge schema into root");
    }

    return result;
}
