#ifndef TBE_COMPILER_CORE_H
#define TBE_COMPILER_CORE_H

#include "node_tree.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tbe_compiler_options_s {
  const char *schema_path;
  const char *template_path;
  const char *output_path;
  const char *dsl_output_path;
  int64_t lang_enum;
} tbe_compiler_options_t;

enum {
  TBE_COMPILER_LANG_C = 0,
  TBE_COMPILER_LANG_PYTHON = 1,
  TBE_COMPILER_LANG_RUST = 2,
  TBE_COMPILER_LANG_CPP = 3,
  TBE_COMPILER_LANG_GO = 4,
  TBE_COMPILER_LANG_TS = 5,
  TBE_COMPILER_LANG_MIR = 6,
  TBE_COMPILER_LANG_BMIR = 7
};

char *tbe_compiler_read_file(const char *filename);

const char *tbe_compiler_resolve_template(const char *user_template,
                                          int64_t lang_enum);

int tbe_compiler_parse_schema_file(const char *schema_path, Node **out_root,
                                   char **out_schema_data);

int tbe_compiler_render_file(Node *root, const char *template_path,
                             const char *output_path);

int tbe_compiler_run(const tbe_compiler_options_t *options);

#ifdef __cplusplus
}
#endif

#endif
