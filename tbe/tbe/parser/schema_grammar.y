/**
 * @file schema_grammar.y
 * @brief Schema Grammar for tbe_compiler (Lemon)
 *
 * Supports the TBE-like declarations:
 *   schema Market [id(7), version(2), byte_order(little)];
 *   composite Header { uint32 seq; }
 *   group Level { uint64 price; uint32 qty; }
 *   [id(100)] message BookSnapshot { group<Level> bids; string symbol; }
 */

%name SchemaParse
%token_prefix SCHEMA_TOKEN_
%token_type {schema_token_t}
%default_type {schema_token_t}

%extra_argument {schema_parse_ctx_t *ctx}

%include {
#include "schema_lexer.h"
#include "schema_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *tok_strdup(schema_token_t t) {
    if (!t.value || t.length == 0) {
        char *empty = (char *)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    
    // Prevent integer overflow
    if (t.length > SIZE_MAX - 1) {
        return NULL;
    }
    
    char *s = (char *)malloc(t.length + 1);
    if (!s) return NULL;
    
    memcpy(s, t.value, t.length);
    s[t.length] = '\0';
    return s;
}

static int tok_to_ull(schema_token_t t, unsigned long long *out) {
    char *text = tok_strdup(t);
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    int ok = (text[0] != '\0' && end && *end == '\0');

    free(text);
    if (!ok) {
        return 0;
    }

    *out = value;
    return 1;
}

static int is_numeric_literal(const char *text) {
    if (!text || !text[0]) {
        return 0;
    }

    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

static void add_true(Node *map, const char *name) {
    map_add(map, create_node_string(name, "1"));
}

static void add_name_nodes(Node *map, const char *specific_key, const char *name) {
    map_add(map, create_node_string(specific_key, name));
    map_add(map, create_node_string("name", name));
}

static const char *map_get_string_value(Node *map, const char *name) {
    if (!map || map->type != NODE_MAP) {
        return NULL;
    }

    for (size_t i = 0; i < map->data.map.count; ++i) {
        Node *child = map->data.map.items[i];
        if (!child || child->type != NODE_STRING || !child->name) {
            continue;
        }
        if (strcmp(child->name, name) == 0) {
            return child->data.string_val;
        }
    }

    return NULL;
}

static void mark_record_kind(Node *record, schema_record_kind_t kind) {
    switch (kind) {
    case SCHEMA_RECORD_COMPOSITE:
        map_add(record, create_node_string("decl_kind", "composite"));
        add_true(record, "is_composite_decl");
        break;
    case SCHEMA_RECORD_GROUP:
        map_add(record, create_node_string("decl_kind", "group"));
        add_true(record, "is_group_decl");
        break;
    case SCHEMA_RECORD_MESSAGE:
        map_add(record, create_node_string("decl_kind", "message"));
        add_true(record, "is_message_decl");
        break;
    case SCHEMA_RECORD_UNION:
        map_add(record, create_node_string("decl_kind", "union"));
        add_true(record, "is_union_decl");
        break;
    default:
        fprintf(stderr, "schema_grammar: internal error unknown record kind\n");
        abort();
    }
}

static void begin_record(schema_parse_ctx_t *ctx, Node *list,
                         schema_record_kind_t kind,
                         const char *name_key, const char *name) {
    Node *new_record = create_node_map(NULL);
    if (!new_record) {
        ctx->error = 1;
        return;
    }
    
    Node *new_fields = create_node_list("fields");
    if (!new_fields) {
        node_free(new_record);
        ctx->error = 1;
        return;
    }
    
    // Only update context if all allocations succeed
    ctx->cur_record = new_record;
    ctx->cur_record_kind = kind;
    ctx->cur_field_section = SCHEMA_FIELD_SECTION_FIXED;
    ctx->cur_fields = new_fields;

    add_name_nodes(ctx->cur_record, name_key, name);
    mark_record_kind(ctx->cur_record, kind);

    if (map_add(ctx->cur_record, ctx->cur_fields) != 0 ||
        list_add(list, ctx->cur_record) != 0) {
        ctx->error = 1;
        return;
    }
}

static schema_field_section_t classify_field_section(const char *field_type,
                                                     int is_collection,
                                                     int is_group_field,
                                                     const char *length_field) {
    if (is_group_field) {
        return SCHEMA_FIELD_SECTION_GROUP;
    }

    if (strcmp(field_type, "string") == 0) {
        return SCHEMA_FIELD_SECTION_VAR_DATA;
    }

    if (strcmp(field_type, "bytes") == 0 && !is_numeric_literal(length_field)) {
        return SCHEMA_FIELD_SECTION_VAR_DATA;
    }

    if (strcmp(field_type, "array") == 0 && is_numeric_literal(length_field)) {
        return SCHEMA_FIELD_SECTION_FIXED;
    }

    if (is_collection) {
        return SCHEMA_FIELD_SECTION_VAR_DATA;
    }

    return SCHEMA_FIELD_SECTION_FIXED;
}

static int field_supported_in_tbe(const char *field_type,
                                  int is_collection,
                                  int is_group_field,
                                  const char *length_field) {
    if (is_group_field) {
        return 1;
    }

    if (!is_collection) {
        return 1;
    }

    if (strcmp(field_type, "array") == 0) {
        return is_numeric_literal(length_field);
    }

    return strcmp(field_type, "list") == 0 ||
           strcmp(field_type, "set") == 0 ||
           strcmp(field_type, "map") == 0;
}

static int validate_field_layout(schema_parse_ctx_t *ctx,
                                 const char *field_type,
                                 int is_collection,
                                 int is_group_field,
                                 const char *length_field) {
    schema_field_section_t section;

    /* Union variants are user-defined type references — no layout constraints */
    if (ctx->cur_record_kind == SCHEMA_RECORD_UNION) {
        return 1;
    }

    if (!field_supported_in_tbe(field_type, is_collection, is_group_field, length_field)) {
        fprintf(stderr, "schema_grammar: unsupported dynamic collection in tbe declaration\n");
        ctx->error = 1;
        return 0;
    }

    section = classify_field_section(field_type, is_collection, is_group_field, length_field);
    if (ctx->cur_record_kind == SCHEMA_RECORD_COMPOSITE &&
        section != SCHEMA_FIELD_SECTION_FIXED) {
        fprintf(stderr, "schema_grammar: composite fields must be fixed-size\n");
        ctx->error = 1;
        return 0;
    }

    if (ctx->cur_record_kind == SCHEMA_RECORD_MESSAGE ||
        ctx->cur_record_kind == SCHEMA_RECORD_GROUP) {
        if (section > ctx->cur_field_section) {
            ctx->cur_field_section = section;
        }
    }

    return 1;
}

static void annotate_field(Node *field_map, const char *field_type,
                           int is_collection, const char *collection_inner,
                           const char *length_field, int is_group_field) {
    int size = 0;
    int is_numeric = 0;
    int is_unsigned = 0;
    int is_uuid = 0;
    const char *host_type = NULL;
    const char *wire_reader = NULL;
    const char *map_value_type = NULL;

    if (is_group_field) {
        map_add(field_map, create_node_string("ctype", "GROUP"));
        add_true(field_map, "is_group_field");
        add_true(field_map, "is_variable_size");
        if (collection_inner && collection_inner[0]) {
            map_add(field_map, create_node_string("group_type", collection_inner));
            map_add(field_map, create_node_string("inner_type", collection_inner));
        }
    } else if (strcmp(field_type, "varint") == 0) {
        map_add(field_map, create_node_string("ctype", "VARINT"));
        add_true(field_map, "is_varint");
        add_true(field_map, "is_variable_size");
    } else if (strcmp(field_type, "uint8_t") == 0 || strcmp(field_type, "uint8") == 0 ||
               strcmp(field_type, "byte") == 0) {
        size = 1; is_numeric = 1; is_unsigned = 1;
        host_type = "uint8_t"; wire_reader = "u8";
    } else if (strcmp(field_type, "int8_t") == 0 || strcmp(field_type, "int8") == 0) {
        size = 1; is_numeric = 1;
        host_type = "int8_t"; wire_reader = "i8";
    } else if (strcmp(field_type, "uint16_t") == 0 || strcmp(field_type, "uint16") == 0) {
        size = 2; is_numeric = 1; is_unsigned = 1;
        host_type = "uint16_t"; wire_reader = "u16";
    } else if (strcmp(field_type, "int16_t") == 0 || strcmp(field_type, "int16") == 0) {
        size = 2; is_numeric = 1;
        host_type = "int16_t"; wire_reader = "i16";
    } else if (strcmp(field_type, "uint32_t") == 0 || strcmp(field_type, "uint32") == 0) {
        size = 4; is_numeric = 1; is_unsigned = 1;
        host_type = "uint32_t"; wire_reader = "u32";
    } else if (strcmp(field_type, "int32_t") == 0 || strcmp(field_type, "int32") == 0) {
        size = 4; is_numeric = 1;
        host_type = "int32_t"; wire_reader = "i32";
    } else if (strcmp(field_type, "uint64_t") == 0 || strcmp(field_type, "uint64") == 0) {
        size = 8; is_numeric = 1; is_unsigned = 1;
        host_type = "uint64_t"; wire_reader = "u64";
    } else if (strcmp(field_type, "int64_t") == 0 || strcmp(field_type, "int64") == 0) {
        size = 8; is_numeric = 1;
        host_type = "int64_t"; wire_reader = "i64";
    } else if (strcmp(field_type, "float") == 0) {
        size = 4; is_numeric = 1;
        host_type = "float"; wire_reader = "f32";
    } else if (strcmp(field_type, "double") == 0) {
        size = 8; is_numeric = 1;
        host_type = "double"; wire_reader = "f64";
    } else if (strcmp(field_type, "uuid") == 0) {
        size = 16; is_uuid = 1;
    }

    if (is_numeric) {
        char size_text[16];
        snprintf(size_text, sizeof(size_text), "%d", size);
        map_add(field_map, create_node_string("size_bytes", size_text));
        if (host_type) {
            map_add(field_map, create_node_string("host_type", host_type));
        }
        if (wire_reader) {
            map_add(field_map, create_node_string("wire_reader", wire_reader));
        }
        add_true(field_map, "is_numeric");
        if (is_unsigned) {
            add_true(field_map, "is_unsigned");
        }
        add_true(field_map, "is_primitive");
        add_true(field_map, "is_fixed_size");
    }

    if (is_uuid) {
        char size_text[16];
        snprintf(size_text, sizeof(size_text), "%d", size);
        map_add(field_map, create_node_string("ctype", "UUID"));
        map_add(field_map, create_node_string("size_bytes", size_text));
        add_true(field_map, "is_uuid");
        add_true(field_map, "is_primitive");
        add_true(field_map, "is_fixed_size");
    }

    if (!is_group_field && strstr(field_type, "int") != NULL) {
        add_true(field_map, "is_integer");
    } else if (!is_group_field &&
               (strcmp(field_type, "float") == 0 || strcmp(field_type, "double") == 0)) {
        add_true(field_map, "is_float");
    } else if (!is_group_field && strcmp(field_type, "bytes") == 0) {
        map_add(field_map, create_node_string("ctype", "BYTES"));
        add_true(field_map, "is_bytes");
        if (is_numeric_literal(length_field)) {
            map_add(field_map, create_node_string("size_bytes", length_field));
            add_true(field_map, "is_fixed_size");
        } else {
            add_true(field_map, "is_variable_size");
        }
    } else if (!is_group_field && strcmp(field_type, "string") == 0) {
        map_add(field_map, create_node_string("ctype", "STRING"));
        add_true(field_map, "is_string");
        add_true(field_map, "is_variable_size");
    } else if (!is_group_field && (is_collection || strcmp(field_type, "array") == 0 ||
               strcmp(field_type, "list") == 0 || strcmp(field_type, "set") == 0 ||
               strcmp(field_type, "map") == 0)) {
        map_add(field_map, create_node_string("ctype", "COLLECTION"));
        add_true(field_map, "is_collection");
        map_add(field_map, create_node_string("collection_kind", field_type));
        if (strcmp(field_type, "list") == 0) {
            add_true(field_map, "is_list");
        } else if (strcmp(field_type, "set") == 0) {
            add_true(field_map, "is_set");
        } else if (strcmp(field_type, "map") == 0) {
            add_true(field_map, "is_map");
        }
        if (strcmp(field_type, "map") == 0 && collection_inner && collection_inner[0]) {
            map_value_type = strchr(collection_inner, ',');
            if (map_value_type != NULL) {
                size_t key_len = (size_t)(map_value_type - collection_inner);
                char key_type[128];
                char value_type[128];

                if (key_len < sizeof(key_type)) {
                    memcpy(key_type, collection_inner, key_len);
                    key_type[key_len] = '\0';
                    map_add(field_map, create_node_string("key_type", key_type));
                }
                snprintf(value_type, sizeof(value_type), "%s", map_value_type + 1);
                map_add(field_map, create_node_string("value_type", value_type));
                map_add(field_map, create_node_string("inner_type", value_type));
            }
        } else if (collection_inner && collection_inner[0]) {
            map_add(field_map, create_node_string("inner_type", collection_inner));
        }
        if (is_numeric_literal(length_field)) {
            add_true(field_map, "is_fixed_size");
        } else {
            add_true(field_map, "is_variable_size");
        }
    } else if (!is_group_field && !is_numeric) {
        map_add(field_map, create_node_string("ctype", "USER_DEFINED"));
        add_true(field_map, "is_user_defined");
    }

    if (length_field && length_field[0]) {
        map_add(field_map, create_node_string("length_field", length_field));
        add_true(field_map, "has_length_field");
    }

    if (is_group_field) {
        /* group fields are their own section */
    } else if (strcmp(field_type, "string") == 0 ||
               (strcmp(field_type, "bytes") == 0 && !is_numeric_literal(length_field))) {
        add_true(field_map, "is_var_data");
    } else if (!is_collection ||
               (strcmp(field_type, "array") == 0 && is_numeric_literal(length_field)) ||
               (strcmp(field_type, "bytes") == 0 && is_numeric_literal(length_field))) {
        add_true(field_map, "is_fixed_block");
    }
}

static void add_field(schema_parse_ctx_t *ctx,
                      const char *type_str, const char *name_str,
                      int is_collection, const char *inner, const char *len_field,
                      Node *attrs, int is_group_field, int is_optional, const char *default_value) {
    Node *field_map;

    if (!validate_field_layout(ctx, type_str, is_collection, is_group_field, len_field)) {
        return;
    }

    field_map = create_node_map(NULL);
    map_add(field_map, create_node_string("type", type_str));
    map_add(field_map, create_node_string("name", name_str));
    map_add(field_map, create_node_string("owner_name",
                                          map_get_string_value(ctx->cur_record, "name")));
    
    if (is_optional) {
        add_true(field_map, "is_optional");
    }
    
    if (default_value && default_value[0] != '\0') {
        map_add(field_map, create_node_string("default_value", default_value));
        add_true(field_map, "has_default");
    }

    if (attrs != NULL) {
        map_add(field_map, attrs);
    }
    
    annotate_field(field_map, type_str, is_collection, inner, len_field, is_group_field);
    list_add(ctx->cur_fields, field_map);
}

static Node *create_attribute_node(schema_token_t key_tok, schema_token_t value_tok) {
    char *key = tok_strdup(key_tok);
    char *value = tok_strdup(value_tok);
    Node *attr = create_node_map(key);

    map_add(attr, create_node_string("name", key));
    map_add(attr, create_node_string("value", value));

    free(key);
    free(value);
    return attr;
}

static void add_enum_item(schema_parse_ctx_t *ctx, const char *key, const char *value) {
    Node *item = create_node_map(NULL);
    map_add(item, create_node_string("name", key));
    map_add(item, create_node_string("value", value));
    list_add(ctx->cur_enum_items, item);
}
}

%type attribute_list {Node *}
%type attr_items {Node *}
%type attr_item {Node *}
%type field_default {char *}
%type field_qualifier {int}
%destructor attribute_list { node_free($$); }
%destructor attr_items { node_free($$); }
%destructor field_default { free($$); }
%destructor attr_item { node_free($$); }

%token ENUM FLAGS NUMBER EQUALS IDENT LBRACE RBRACE SEMI LPAREN RPAREN LBRACKET RBRACKET LT GT COMMA MESSAGE COMPOSITE GROUP SCHEMA REQUIRED OPTIONAL DEFAULT STRING TRUE FALSE UNION.

start ::= schema.
schema ::= decl_list.

decl_list ::= decl_list decl.
decl_list ::= .

decl ::= enum_decl.
decl ::= flags_decl.
decl ::= message_decl.
decl ::= composite_decl.
decl ::= group_decl.
decl ::= schema_decl.
decl ::= union_decl.

attribute_list(A) ::= LBRACKET attr_items(B) RBRACKET. { A = B; }
attribute_list(A) ::= . { A = NULL; }

attr_items(A) ::= attr_items(B) COMMA attr_item(C). {
    A = B;
    map_add(A, C);
}
attr_items(A) ::= attr_item(B). {
    A = create_node_list("attributes");
    list_add(A, B);
}

attr_item(A) ::= IDENT(K) LPAREN IDENT(V) RPAREN. { A = create_attribute_node(K, V); }
attr_item(A) ::= IDENT(K) LPAREN NUMBER(V) RPAREN. { A = create_attribute_node(K, V); }

schema_decl ::= SCHEMA IDENT(N) attribute_list(A) SEMI. {
    if (ctx->schema_node != NULL) {
        node_free(A);
        fprintf(stderr, "schema_grammar: duplicate schema declaration\n");
        ctx->error = 1;
    } else {
        char *schema_name = tok_strdup(N);
        ctx->schema_node = create_node_map("schema");
        add_name_nodes(ctx->schema_node, "schema_name", schema_name);
        if (A) {
            map_add(ctx->schema_node, A);
        }
        map_add(ctx->root, ctx->schema_node);
        free(schema_name);
    }
}

enum_decl ::= attribute_list(A) enum_header enum_body RBRACE. {
    if (A) {
        map_add(ctx->cur_enum, A);
    }
}

enum_header ::= ENUM IDENT(N) LBRACE. {
    char *enum_name = tok_strdup(N);
    ctx->cur_enum = create_node_map(NULL);
    ctx->next_enum_value = 0;
    add_name_nodes(ctx->cur_enum, "enum_name", enum_name);
    ctx->cur_enum_items = create_node_list("items");
    map_add(ctx->cur_enum, ctx->cur_enum_items);
    list_add(ctx->enums_list, ctx->cur_enum);
    free(enum_name);
}

enum_header ::= ENUM IDENT(N) LT IDENT(T) GT LBRACE. {
    char *enum_name = tok_strdup(N);
    char *underlying_type = tok_strdup(T);
    ctx->cur_enum = create_node_map(NULL);
    ctx->next_enum_value = 0;
    add_name_nodes(ctx->cur_enum, "enum_name", enum_name);
    map_add(ctx->cur_enum, create_node_string("underlying_type", underlying_type));
    ctx->cur_enum_items = create_node_list("items");
    map_add(ctx->cur_enum, ctx->cur_enum_items);
    list_add(ctx->enums_list, ctx->cur_enum);
    free(enum_name);
    free(underlying_type);
}

enum_body ::= enum_body enum_item.
enum_body ::= .

enum_item ::= IDENT(K) EQUALS NUMBER(V) SEMI. {
    char *key = tok_strdup(K);
    char *value = tok_strdup(V);
    unsigned long long next_value = 0;

    add_enum_item(ctx, key, value);
    if (tok_to_ull(V, &next_value)) {
        ctx->next_enum_value = next_value + 1;
    }

    free(key);
    free(value);
}

enum_item ::= IDENT(K) SEMI. {
    char *key = tok_strdup(K);
    char value_buf[32];

    snprintf(value_buf, sizeof(value_buf), "%llu", ctx->next_enum_value);
    add_enum_item(ctx, key, value_buf);
    ctx->next_enum_value++;

    free(key);
}

// Flags declarations (similar to enum but with is_flags marker)
flags_decl ::= attribute_list(A) flags_header flags_body RBRACE. {
    if (A) {
        map_add(ctx->cur_enum, A);
    }
}

flags_header ::= FLAGS IDENT(N) LBRACE. {
    char *flags_name = tok_strdup(N);
    ctx->cur_enum = create_node_map(NULL);
    ctx->next_enum_value = 1;  // Start at 1 for flags (power of 2)
    add_name_nodes(ctx->cur_enum, "enum_name", flags_name);
    map_add(ctx->cur_enum, create_node_string("is_flags", "1"));
    ctx->cur_enum_items = create_node_list("items");
    map_add(ctx->cur_enum, ctx->cur_enum_items);
    list_add(ctx->enums_list, ctx->cur_enum);
    free(flags_name);
}

flags_header ::= FLAGS IDENT(N) LT IDENT(T) GT LBRACE. {
    char *flags_name = tok_strdup(N);
    char *underlying_type = tok_strdup(T);
    ctx->cur_enum = create_node_map(NULL);
    ctx->next_enum_value = 1;  // Start at 1 for flags
    add_name_nodes(ctx->cur_enum, "enum_name", flags_name);
    map_add(ctx->cur_enum, create_node_string("underlying_type", underlying_type));
    map_add(ctx->cur_enum, create_node_string("is_flags", "1"));
    ctx->cur_enum_items = create_node_list("items");
    map_add(ctx->cur_enum, ctx->cur_enum_items);
    list_add(ctx->enums_list, ctx->cur_enum);
    free(flags_name);
    free(underlying_type);
}

flags_body ::= flags_body flags_item.
flags_body ::= .

flags_item ::= IDENT(K) EQUALS NUMBER(V) SEMI. {
    char *key = tok_strdup(K);
    char *value = tok_strdup(V);
    unsigned long long next_value = 0;

    add_enum_item(ctx, key, value);
    if (tok_to_ull(V, &next_value)) {
        // For flags, next value is next power of 2
        ctx->next_enum_value = next_value << 1;
    }

    free(key);
    free(value);
}

flags_item ::= IDENT(K) SEMI. {
    char *key = tok_strdup(K);
    char value_buf[32];

    snprintf(value_buf, sizeof(value_buf), "%llu", ctx->next_enum_value);
    add_enum_item(ctx, key, value_buf);
    // Next power of 2
    ctx->next_enum_value = ctx->next_enum_value << 1;

    free(key);
}

composite_decl ::= attribute_list(A) composite_header field_list RBRACE. {
    if (A) {
        map_add(ctx->cur_record, A);
    }
}

union_decl ::= attribute_list(A) union_header union_body RBRACE. {
    if (A) {
        map_add(ctx->cur_record, A);
    }
}

union_header ::= UNION IDENT(N) LBRACE. {
    char *record_name = tok_strdup(N);
    begin_record(ctx, ctx->unions_list, SCHEMA_RECORD_UNION, "union_name", record_name);
    free(record_name);
}

union_body ::= union_body union_variant.
union_body ::= .

union_variant ::= IDENT(T) IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *field_name = tok_strdup(N);
    add_field(ctx, type_name, field_name, 0, "", "", NULL, 0, 0, NULL);
    free(type_name);
    free(field_name);
}

composite_header ::= COMPOSITE IDENT(N) LBRACE. {
    char *record_name = tok_strdup(N);
    begin_record(ctx, ctx->composites_list, SCHEMA_RECORD_COMPOSITE, "composite_name",
                 record_name);
    free(record_name);
}

group_decl ::= attribute_list(A) group_header field_list RBRACE. {
    if (A) {
        map_add(ctx->cur_record, A);
    }
}

group_header ::= GROUP IDENT(N) LBRACE. {
    char *record_name = tok_strdup(N);
    begin_record(ctx, ctx->groups_list, SCHEMA_RECORD_GROUP, "group_name", record_name);
    free(record_name);
}

message_decl ::= attribute_list(A) message_header field_list RBRACE. {
    if (A) {
        map_add(ctx->cur_record, A);
    }
}

message_header ::= MESSAGE IDENT(N) LBRACE. {
    char *record_name = tok_strdup(N);
    begin_record(ctx, ctx->messages_list, SCHEMA_RECORD_MESSAGE, "message_name",
                 record_name);
    free(record_name);
}

field_list ::= field_list field_decl.
field_list ::= .

field_decl ::= field_qualifier(Q) attribute_list(A) IDENT(T) IDENT(N) field_default(D) SEMI. {
    char *type_name = tok_strdup(T);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == 1);
    add_field(ctx, type_name, field_name, 0, "", "", A, 0, is_optional, D);
    free(type_name);
    free(field_name);
    if (D) free(D);
}

field_default(D) ::= DEFAULT NUMBER(V). { D = tok_strdup(V); }
field_default(D) ::= DEFAULT STRING(V). { D = tok_strdup(V); }
field_default(D) ::= DEFAULT TRUE(V). { D = tok_strdup(V); }
field_default(D) ::= DEFAULT FALSE(V). { D = tok_strdup(V); }
field_default(D) ::= DEFAULT IDENT(V). { D = tok_strdup(V); }
field_default(D) ::= . { D = NULL; }

field_decl ::= field_qualifier(Q) attribute_list(A) IDENT(T) LPAREN IDENT(L) RPAREN IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *length_field = tok_strdup(L);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == 1);
    add_field(ctx, type_name, field_name, 0, "", length_field, A, 0, is_optional, NULL);
    free(type_name);
    free(length_field);
    free(field_name);
}

field_decl ::= field_qualifier(Q) attribute_list(A) IDENT(T) LPAREN NUMBER(L) RPAREN IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *length_field = tok_strdup(L);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == 1);
    add_field(ctx, type_name, field_name, 0, "", length_field, A, 0, is_optional, NULL);
    free(type_name);
    free(length_field);
    free(field_name);
}

field_decl ::= field_qualifier(Q) attribute_list(A) GROUP LT IDENT(I) GT IDENT(N) SEMI. {
    char *group_type = tok_strdup(I);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == 1);
    add_field(ctx, "group", field_name, 0, group_type, "", A, 1, is_optional, NULL);
    free(group_type);
    free(field_name);
}

field_decl ::= field_qualifier(Q) attribute_list(A) IDENT(T) LBRACKET IDENT(L) RBRACKET IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *length_field = tok_strdup(L);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == 1);
    add_field(ctx, "array", field_name, 1, type_name, length_field, A, 0, is_optional, NULL);
    free(type_name);
    free(length_field);
    free(field_name);
}

field_decl ::= field_qualifier(Q) attribute_list(A) IDENT(T) LBRACKET NUMBER(L) RBRACKET IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *length_field = tok_strdup(L);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == 1);
    add_field(ctx, "array", field_name, 1, type_name, length_field, A, 0, is_optional, NULL);
    free(type_name);
    free(length_field);
    free(field_name);
}

field_decl ::= field_qualifier(Q) attribute_list(A) IDENT(T) LT IDENT(I) GT IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *inner_type = tok_strdup(I);
    char *field_name = tok_strdup(N);
    int is_optional = (Q == 1);
    add_field(ctx, type_name, field_name, 1, inner_type, "", A, 0, is_optional, NULL);
    free(type_name);
    free(inner_type);
    free(field_name);
}

field_decl ::= field_qualifier(Q) attribute_list(A) IDENT(T) LT IDENT(K) COMMA IDENT(V) GT IDENT(N) SEMI. {
    char *type_name = tok_strdup(T);
    char *field_name = tok_strdup(N);
    char *key_type = tok_strdup(K);
    char *value_type = tok_strdup(V);
    char map_inner[256];
    int is_optional = (Q == 1);

    snprintf(map_inner, sizeof(map_inner), "%s,%s", key_type, value_type);
    add_field(ctx, type_name, field_name, 1, map_inner, "", A, 0, is_optional, NULL);
    free(type_name);
    free(field_name);
    free(key_type);
    free(value_type);
}

field_qualifier(Q) ::= REQUIRED. { Q = 0; }  // 0 = required
field_qualifier(Q) ::= OPTIONAL. { Q = 1; }  // 1 = optional 
field_qualifier(Q) ::= .         { Q = 0; }  // default = required

%syntax_error {
    // TOKEN is the current token that caused the error
    ctx->error_line = TOKEN.line;
    ctx->error_column = TOKEN.column;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "Syntax error at line %d, column %d",
             TOKEN.line, TOKEN.column);
    fprintf(stderr, "%s\n", ctx->error_msg);
    ctx->error = 1;
}

%parse_failure {
    if (!ctx->error) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Parse failure: unable to recover from syntax errors");
        fprintf(stderr, "%s\n", ctx->error_msg);
    }
    ctx->error = 1;
}
