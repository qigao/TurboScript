#include "compiler_core.h"

#include "mustache.h"
#include "mustache_helpers.h"
#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "data_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Node *tbe_compiler_find_child(Node *parent, const char *name) {
  if (!parent || !name) return NULL;

  if (parent->type == NODE_MAP) {
    for (size_t i = 0; i < parent->data.map.count; ++i) {
      Node *child = parent->data.map.items[i];
      if (child && child->name && strcmp(child->name, name) == 0) return child;
    }
  } else if (parent->type == NODE_LIST) {
    for (size_t i = 0; i < parent->data.list.count; ++i) {
      Node *child = parent->data.list.items[i];
      if (child && child->name && strcmp(child->name, name) == 0) return child;
    }
  }

  return NULL;
}

static const char *tbe_compiler_string_value(Node *parent, const char *name) {
  Node *child = tbe_compiler_find_child(parent, name);
  if (!child || child->type != NODE_STRING) return NULL;
  return child->data.string_val;
}

static int tbe_compiler_has_child(Node *parent, const char *name) {
  return tbe_compiler_find_child(parent, name) != NULL;
}

static void tbe_compiler_remove_children(Node *map, const char *name) {
  size_t out = 0;

  if (!map || map->type != NODE_MAP || !name) return;

  for (size_t i = 0; i < map->data.map.count; ++i) {
    Node *child = map->data.map.items[i];
    if (child && child->name && strcmp(child->name, name) == 0) {
      node_free(child);
      continue;
    }
    map->data.map.items[out++] = child;
  }

  map->data.map.count = out;
}

static int tbe_compiler_set_string(Node *map, const char *name, const char *value) {
  Node *node;

  if (!map || !name || !value) return -1;

  node = create_node_string(name, value);
  if (!node) return -1;

  tbe_compiler_remove_children(map, name);
  if (map_add(map, node) != 0) {
    node_free(node);
    return -1;
  }

  return 0;
}

static void tbe_compiler_pascal_identifier(const char *input, char *out, size_t out_size) {
  int capitalize = 1;
  size_t pos = 0;

  if (!out || out_size == 0) return;

  if (!input || !input[0]) {
    snprintf(out, out_size, "Field");
    return;
  }

  for (size_t i = 0; input[i] && pos + 1 < out_size; ++i) {
    char c = input[i];
    int is_alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    int is_digit = c >= '0' && c <= '9';

    if (!is_alpha && !is_digit) {
      capitalize = 1;
      continue;
    }

    if (pos == 0 && is_digit && pos + 1 < out_size) {
      out[pos++] = 'F';
    }

    if (capitalize && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    out[pos++] = c;
    capitalize = 0;
  }

  if (pos == 0) out[pos++] = 'F';
  out[pos] = '\0';
}

static const char *tbe_compiler_cpp_scalar_type(const char *type) {
  if (!type) return "std::any";
  if (strcmp(type, "bool") == 0) return "bool";
  if (strcmp(type, "byte") == 0 || strcmp(type, "uint8") == 0 || strcmp(type, "uint8_t") == 0) return "std::uint8_t";
  if (strcmp(type, "int8") == 0 || strcmp(type, "int8_t") == 0) return "std::int8_t";
  if (strcmp(type, "uint16") == 0 || strcmp(type, "uint16_t") == 0) return "std::uint16_t";
  if (strcmp(type, "int16") == 0 || strcmp(type, "int16_t") == 0) return "std::int16_t";
  if (strcmp(type, "uint32") == 0 || strcmp(type, "uint32_t") == 0) return "std::uint32_t";
  if (strcmp(type, "int32") == 0 || strcmp(type, "int32_t") == 0) return "std::int32_t";
  if (strcmp(type, "uint64") == 0 || strcmp(type, "uint64_t") == 0) return "std::uint64_t";
  if (strcmp(type, "int64") == 0 || strcmp(type, "int64_t") == 0) return "std::int64_t";
  if (strcmp(type, "float") == 0) return "float";
  if (strcmp(type, "double") == 0) return "double";
  if (strcmp(type, "string") == 0) return "std::string";
  if (strcmp(type, "bytes") == 0) return "std::vector<std::uint8_t>";
  return type;
}

static const char *tbe_compiler_go_scalar_type(const char *type) {
  if (!type) return "any";
  if (strcmp(type, "bool") == 0) return "bool";
  if (strcmp(type, "byte") == 0 || strcmp(type, "uint8") == 0 || strcmp(type, "uint8_t") == 0) return "uint8";
  if (strcmp(type, "int8") == 0 || strcmp(type, "int8_t") == 0) return "int8";
  if (strcmp(type, "uint16") == 0 || strcmp(type, "uint16_t") == 0) return "uint16";
  if (strcmp(type, "int16") == 0 || strcmp(type, "int16_t") == 0) return "int16";
  if (strcmp(type, "uint32") == 0 || strcmp(type, "uint32_t") == 0) return "uint32";
  if (strcmp(type, "int32") == 0 || strcmp(type, "int32_t") == 0) return "int32";
  if (strcmp(type, "uint64") == 0 || strcmp(type, "uint64_t") == 0) return "uint64";
  if (strcmp(type, "int64") == 0 || strcmp(type, "int64_t") == 0) return "int64";
  if (strcmp(type, "float") == 0) return "float32";
  if (strcmp(type, "double") == 0) return "float64";
  if (strcmp(type, "string") == 0) return "string";
  if (strcmp(type, "bytes") == 0) return "[]byte";
  return type;
}

static const char *tbe_compiler_ts_scalar_type(const char *type) {
  if (!type) return "unknown";
  if (strcmp(type, "bool") == 0) return "boolean";
  if (strstr(type, "int") || strcmp(type, "byte") == 0 ||
      strcmp(type, "float") == 0 || strcmp(type, "double") == 0) {
    return "number";
  }
  if (strcmp(type, "string") == 0) return "string";
  if (strcmp(type, "bytes") == 0) return "Uint8Array";
  return type;
}

static const char *tbe_compiler_python_scalar_type(const char *type) {
  if (!type) return "Any";
  if (strcmp(type, "bool") == 0) return "bool";
  if (strstr(type, "int") || strcmp(type, "byte") == 0) return "int";
  if (strcmp(type, "float") == 0 || strcmp(type, "double") == 0) return "float";
  if (strcmp(type, "string") == 0) return "str";
  if (strcmp(type, "bytes") == 0) return "bytes";
  return type;
}

static const char *tbe_compiler_rust_scalar_type(const char *type) {
  if (!type) return "()";
  if (strcmp(type, "bool") == 0) return "bool";
  if (strcmp(type, "byte") == 0 || strcmp(type, "uint8") == 0 || strcmp(type, "uint8_t") == 0) return "u8";
  if (strcmp(type, "int8") == 0 || strcmp(type, "int8_t") == 0) return "i8";
  if (strcmp(type, "uint16") == 0 || strcmp(type, "uint16_t") == 0) return "u16";
  if (strcmp(type, "int16") == 0 || strcmp(type, "int16_t") == 0) return "i16";
  if (strcmp(type, "uint32") == 0 || strcmp(type, "uint32_t") == 0) return "u32";
  if (strcmp(type, "int32") == 0 || strcmp(type, "int32_t") == 0) return "i32";
  if (strcmp(type, "uint64") == 0 || strcmp(type, "uint64_t") == 0) return "u64";
  if (strcmp(type, "int64") == 0 || strcmp(type, "int64_t") == 0) return "i64";
  if (strcmp(type, "float") == 0) return "f32";
  if (strcmp(type, "double") == 0) return "f64";
  if (strcmp(type, "string") == 0) return "String";
  if (strcmp(type, "bytes") == 0) return "Vec<u8>";
  return type;
}

static const char *tbe_compiler_cpp_enum_underlying_type(const char *type) {
  return tbe_compiler_cpp_scalar_type((type && type[0]) ? type : "int32");
}

static const char *tbe_compiler_rust_enum_underlying_type(const char *type) {
  return tbe_compiler_rust_scalar_type((type && type[0]) ? type : "int32");
}

typedef const char *(*tbe_scalar_mapper_t)(const char *);

static void tbe_compiler_field_type(Node *field,
                                    tbe_scalar_mapper_t scalar_mapper,
                                    const char *list_prefix,
                                    const char *list_suffix,
                                    const char *set_prefix,
                                    const char *set_suffix,
                                    const char *map_prefix,
                                    const char *map_separator,
                                    const char *map_suffix,
                                    char *out,
                                    size_t out_size) {
  const char *type = tbe_compiler_string_value(field, "type");

  if (!out || out_size == 0) return;

  if (tbe_compiler_has_child(field, "is_group_field")) {
    const char *group_type = tbe_compiler_string_value(field, "group_type");
    snprintf(out, out_size, "%s%s%s", list_prefix, group_type ? group_type : "unknown", list_suffix);
    return;
  }

  if (tbe_compiler_has_child(field, "is_collection")) {
    const char *inner_type = tbe_compiler_string_value(field, "inner_type");
    const char *key_type = tbe_compiler_string_value(field, "key_type");
    const char *value_type = tbe_compiler_string_value(field, "value_type");

    if (tbe_compiler_has_child(field, "is_map")) {
      snprintf(out, out_size, "%s%s%s%s%s", map_prefix,
               scalar_mapper(key_type ? key_type : "string"),
               map_separator,
               scalar_mapper(value_type ? value_type : inner_type),
               map_suffix);
    } else if (tbe_compiler_has_child(field, "is_set")) {
      snprintf(out, out_size, "%s%s%s", set_prefix,
               scalar_mapper(inner_type ? inner_type : "unknown"),
               set_suffix);
    } else {
      snprintf(out, out_size, "%s%s%s", list_prefix,
               scalar_mapper(inner_type ? inner_type : "unknown"),
               list_suffix);
    }
    return;
  }

  if (tbe_compiler_has_child(field, "is_bytes") && tbe_compiler_has_child(field, "is_fixed_size")) {
    snprintf(out, out_size, "%s", scalar_mapper("bytes"));
    return;
  }

  snprintf(out, out_size, "%s", scalar_mapper(type));
}

static void tbe_compiler_annotate_field_types(Node *field) {
  char type_buf[256];
  char field_name[128];
  const char *name = tbe_compiler_string_value(field, "name");

  tbe_compiler_pascal_identifier(name, field_name, sizeof(field_name));
  tbe_compiler_set_string(field, "go_name", field_name);

  tbe_compiler_field_type(field, tbe_compiler_cpp_scalar_type,
                          "std::vector<", ">", "std::set<", ">",
                          "std::map<", ", ", ">", type_buf, sizeof(type_buf));
  tbe_compiler_set_string(field, "cpp_type", type_buf);

  tbe_compiler_field_type(field, tbe_compiler_go_scalar_type,
                          "[]", "", "map[", "]struct{}",
                          "map[", "]", "", type_buf, sizeof(type_buf));
  tbe_compiler_set_string(field, "go_type", type_buf);

  tbe_compiler_field_type(field, tbe_compiler_ts_scalar_type,
                          "Array<", ">", "Set<", ">",
                          "Map<", ", ", ">", type_buf, sizeof(type_buf));
  tbe_compiler_set_string(field, "ts_type", type_buf);

  tbe_compiler_field_type(field, tbe_compiler_python_scalar_type,
                          "list[", "]", "set[", "]",
                          "dict[", ", ", "]", type_buf, sizeof(type_buf));
  tbe_compiler_set_string(field, "python_type", type_buf);

  tbe_compiler_field_type(field, tbe_compiler_rust_scalar_type,
                          "Vec<", ">", "std::collections::HashSet<", ">",
                          "std::collections::HashMap<", ", ", ">", type_buf, sizeof(type_buf));
  tbe_compiler_set_string(field, "rust_type", type_buf);
}

static void tbe_compiler_annotate_record_list_types(Node *root, const char *list_name) {
  Node *list = tbe_compiler_find_child(root, list_name);
  if (!list || list->type != NODE_LIST) return;

  for (size_t i = 0; i < list->data.list.count; ++i) {
    Node *record = list->data.list.items[i];
    Node *fields = tbe_compiler_find_child(record, "fields");
    if (!fields || fields->type != NODE_LIST) continue;

    for (size_t j = 0; j < fields->data.list.count; ++j) {
      tbe_compiler_annotate_field_types(fields->data.list.items[j]);
    }
  }
}

static void tbe_compiler_annotate_enum_types(Node *root) {
  Node *enums = tbe_compiler_find_child(root, "enums");
  if (!enums || enums->type != NODE_LIST) return;

  for (size_t i = 0; i < enums->data.list.count; ++i) {
    Node *enum_node = enums->data.list.items[i];
    const char *underlying = tbe_compiler_string_value(enum_node, "underlying_type");

    tbe_compiler_set_string(enum_node, "cpp_underlying_type",
                            tbe_compiler_cpp_enum_underlying_type(underlying));
    tbe_compiler_set_string(enum_node, "rust_underlying_type",
                            tbe_compiler_rust_enum_underlying_type(underlying));
  }
}

static void tbe_compiler_annotate_schema_types(Node *root) {
  Node *schema = tbe_compiler_find_child(root, "schema");
  const char *schema_name = schema ? tbe_compiler_string_value(schema, "schema_name") : NULL;
  char package_name[128];
  size_t out = 0;

  if (!schema) return;

  if (!schema_name || !schema_name[0]) {
    tbe_compiler_set_string(schema, "go_package_name", "generated");
    return;
  }

  for (size_t i = 0; schema_name[i] && out + 1 < sizeof(package_name); ++i) {
    char c = schema_name[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
      package_name[out++] = c;
    } else if (out > 0 && package_name[out - 1] != '_') {
      package_name[out++] = '_';
    }
  }

  if (out == 0 || (package_name[0] >= '0' && package_name[0] <= '9')) {
    snprintf(package_name, sizeof(package_name), "generated");
  } else {
    package_name[out] = '\0';
  }

  tbe_compiler_set_string(schema, "go_package_name", package_name);
}

static void tbe_compiler_annotate_language_types(Node *root) {
  tbe_compiler_annotate_schema_types(root);
  tbe_compiler_annotate_enum_types(root);
  tbe_compiler_annotate_record_list_types(root, "composites");
  tbe_compiler_annotate_record_list_types(root, "groups");
  tbe_compiler_annotate_record_list_types(root, "messages");
  tbe_compiler_annotate_record_list_types(root, "unions");
}

char *tbe_compiler_read_file(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if (!f) return NULL;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }

  long file_size = ftell(f);
  if (file_size < 0) {
    fclose(f);
    return NULL;
  }

  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }

  size_t size = (size_t)file_size;
  char *dat = (char *)malloc(size + 1);
  if (!dat) {
    fclose(f);
    return NULL;
  }

  size_t bytes_read = fread(dat, 1, size, f);
  fclose(f);
  if (bytes_read != size) {
    free(dat);
    return NULL;
  }

  dat[size] = '\0';
  return dat;
}

const char *tbe_compiler_resolve_template(const char *user_template,
                                          int64_t lang_enum) {
  if (user_template) return user_template;

  switch (lang_enum) {
    default:
    case TBE_COMPILER_LANG_C:
      return "templates/c_structs.mustache";
    case TBE_COMPILER_LANG_PYTHON:
      return "templates/python_dataclass.mustache";
    case TBE_COMPILER_LANG_RUST:
      return "templates/rust_structs.mustache";
    case TBE_COMPILER_LANG_CPP:
      return "templates/cpp_types.mustache";
    case TBE_COMPILER_LANG_GO:
      return "templates/go_types.mustache";
    case TBE_COMPILER_LANG_TS:
      return "templates/ts_types.mustache";
  }
}

static int tbe_compiler_write_file_cb(const void *data, size_t len, void *user) {
  FILE *out = (FILE *)user;
  return out && fwrite(data, 1, len, out) == len ? 0 : -1;
}

static int tbe_compiler_render_mir_file(const char *schema_path, const char *output_path,
                                        int binary_output) {
  FILE *out_file = stdout;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindStatus status;

  if (output_path) {
    out_file = fopen(output_path, "wb");
    if (!out_file) {
      fprintf(stderr, "Failed to open output file: %s\n", output_path);
      return 1;
    }
  }

  status = data_bind_generate_mir(schema_path, tbe_compiler_write_file_cb, out_file,
                                  binary_output, &error);
  if (out_file != stdout) fclose(out_file);
  if (status != DATA_BIND_OK) {
    fprintf(stderr, "Failed to generate MIR output: %s\n",
            error.message[0] != '\0' ? error.message : data_bind_status_name(status));
    return 1;
  }
  return 0;
}

int tbe_compiler_parse_schema_file(const char *schema_path, Node **out_root,
                                   char **out_schema_data) {
  tbe_error_t parse_err;
  Node *root = NULL;
  char *schema_data = tbe_compiler_read_file(schema_path);
  if (!schema_data) {
    fprintf(stderr, "Failed to read schema file: %s\n", schema_path);
    return 1;
  }

  root = create_node_map(NULL);
  if (!root) {
    fprintf(stderr, "Failed to allocate root node\n");
    free(schema_data);
    return 1;
  }

  if (parse_schema(schema_data, strlen(schema_data), root, &parse_err) != 0) {
    if (parse_err.line >= 0) {
      fprintf(stderr, "Parse error at line %d: %s\n", parse_err.line,
              parse_err.message);
    } else {
      fprintf(stderr, "Parse error: %s\n", parse_err.message);
    }
    free(schema_data);
    node_free(root);
    return 1;
  }

  tbe_compiler_annotate_language_types(root);

  *out_root = root;
  *out_schema_data = schema_data;
  return 0;
}

int tbe_compiler_render_file(Node *root, const char *template_path,
                             const char *output_path) {
  MUSTACHE_DATAPROVIDER provider = mustache_helpers_provider();
  MUSTACHE_RENDERER renderer = mustache_helpers_renderer();
  char *templ_data = tbe_compiler_read_file(template_path);
  MUSTACHE_TEMPLATE *templ = NULL;
  FILE *out_file = stdout;
  int res = 1;

  if (!templ_data) {
    fprintf(stderr, "Failed to read template file: %s\n", template_path);
    return 1;
  }

  templ = mustache_compile(templ_data, strlen(templ_data), NULL, NULL, 0);
  if (!templ) {
    fprintf(stderr, "Failed to compile mustache template\n");
    goto cleanup;
  }

  if (output_path) {
    out_file = fopen(output_path, "wb");
    if (!out_file) {
      fprintf(stderr, "Failed to open output file: %s\n", output_path);
      goto cleanup;
    }
  }

  if (mustache_process(templ, &renderer, out_file, &provider, root)
      != MUSTACHE_ERR_SUCCESS) {
    fprintf(stderr, "Failed to render mustache template: %s\n", template_path);
    goto cleanup;
  }

  res = 0;

cleanup:
  if (out_file != stdout) fclose(out_file);
  mustache_release(templ);
  free(templ_data);
  return res;
}

int tbe_compiler_run(const tbe_compiler_options_t *options) {
  Node *root = NULL;
  char *schema_data = NULL;
  int status = tbe_compiler_parse_schema_file(options->schema_path, &root,
                                              &schema_data);
  if (status != 0) return status;

  if (options->template_path == NULL &&
      (options->lang_enum == TBE_COMPILER_LANG_MIR ||
       options->lang_enum == TBE_COMPILER_LANG_BMIR)) {
    status = tbe_compiler_render_mir_file(
        options->schema_path, options->output_path,
        options->lang_enum == TBE_COMPILER_LANG_BMIR);
  } else {
    status = tbe_compiler_render_file(
        root,
        tbe_compiler_resolve_template(options->template_path, options->lang_enum),
        options->output_path);
  }

  if (status == 0 && options->dsl_output_path) {
    if (tbe_compiler_render_file(root, "templates/rfl_types.mustache",
                                 options->dsl_output_path)
        != 0) {
      status = 1;
    }
  }

  free(schema_data);
  node_free(root);
  return status;
}
