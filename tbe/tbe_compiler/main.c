/**
 * @file main.c
 * @brief tbe_compiler — code generator from schema definitions.
 *
 * Reads a .schema file, parses it, and renders output through a Mustache
 * template.  Supports multiple target languages by selecting different
 * template files (built-in or custom).
 *
 * CLI (via cmd_arger):
 *   tbe_compiler <file> [--template <file>] [--lang c|cpp|go|rust|python|py|ts|mir|bmir]
 *              [--output <file>] [--dsl-output <file>]
 */

#include <stdbool.h>
#include <stdio.h>
#include "compiler_core.h"
#include "turbo_parser.h"

int main(int argc, char **argv) {
    char    *schema_path   = NULL;
    char    *template_path = NULL;
    char    *output_path   = NULL;
    char    *dsl_output_path = NULL;
    int64_t  lang_enum     = 0;

    turbo_cmd_parser_t *parser = turbo_cmd_create("tbe_compiler", "1.0");
    
    turbo_cmd_add_required_string(parser, &schema_path, "schema",
                                 "Path to the .schema definition file");
    
    turbo_cmd_add_string(parser, &template_path, "template", "t",
                                 "Path to a custom Mustache template file");
    
    turbo_cmd_enum_t lang_choices[] = {
        { "c",      "C header output",                 TBE_COMPILER_LANG_C },
        { "cpp",    "C++ type output",                 TBE_COMPILER_LANG_CPP },
        { "cxx",    "C++ type output",                 TBE_COMPILER_LANG_CPP },
        { "go",     "Go type output",                  TBE_COMPILER_LANG_GO },
        { "rust",   "Rust struct output",              TBE_COMPILER_LANG_RUST },
        { "python", "Python dataclass output",         TBE_COMPILER_LANG_PYTHON },
        { "py",     "Python dataclass output",         TBE_COMPILER_LANG_PYTHON },
        { "ts",     "TypeScript type output",          TBE_COMPILER_LANG_TS },
        { "typescript", "TypeScript type output",      TBE_COMPILER_LANG_TS },
        { "mir",    "MIR textual IR output",           TBE_COMPILER_LANG_MIR },
        { "bmir",   "MIR binary IR output",            TBE_COMPILER_LANG_BMIR },
    };
    
    turbo_cmd_add_enum(parser, &lang_enum, "lang", "l",
                               "Target language (built-in template)",
                               lang_choices, sizeof(lang_choices) / sizeof(lang_choices[0]));
                               
    turbo_cmd_add_string(parser, &output_path, "output", "o",
                                 "Output file path (default: stdout)");
                                 
    turbo_cmd_add_string(parser, &dsl_output_path, "dsl-output", "d",
                                 "Generate DSL type declarations (.rfl file)");

    turbo_cmd_parse(parser, argc, argv, true);

    tbe_compiler_options_t options = {
        .schema_path = schema_path,
        .template_path = template_path,
        .output_path = output_path,
        .dsl_output_path = dsl_output_path,
        .lang_enum = lang_enum,
    };

    int res = tbe_compiler_run(&options);
    turbo_cmd_destroy(parser);
    return res;
}
