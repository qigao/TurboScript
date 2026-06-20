#include "compiler_core.h"

#include "mustache.h"
#include "mustache_helpers.h"
#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "data_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    case 0:
      return "templates/c_structs.mustache";
    case 1:
      return "templates/python_dataclass.mustache";
    case 2:
      return "templates/rust_structs.mustache";
  }
}

static int tbe_compiler_render_mir_file(const char *schema_path, const char *output_path,
                                        int binary_output) {
  FILE *out_file = stdout;
  char error[256] = {0};
  int status;

  if (output_path) {
    out_file = fopen(output_path, "wb");
    if (!out_file) {
      fprintf(stderr, "Failed to open output file: %s\n", output_path);
      return 1;
    }
  }

  status = data_bind_generate_mir(schema_path, out_file, binary_output, error, sizeof(error));
  if (out_file != stdout) fclose(out_file);
  if (status != 0) {
    fprintf(stderr, "Failed to generate MIR output: %s\n",
            error[0] != '\0' ? error : "unknown error");
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
