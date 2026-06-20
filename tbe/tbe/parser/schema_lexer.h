/**
 * @file schema_lexer.h
 * @brief Schema Lexer Definitions for tbe_compiler
 */

#ifndef SCHEMA_LEXER_H
#define SCHEMA_LEXER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int type;
    const char *value;
    size_t length;
    int line;      // Line number where token starts
    int column;    // Column number where token starts
} schema_token_t;

typedef struct {
    const char *input;
    const char *cursor;
    const char *limit;
    const char *line_start;  // Pointer to start of current line
    int line;
    int column;
} schema_lexer_t;

void schema_lexer_init(schema_lexer_t *lexer, const char *input, size_t length);
int  schema_lexer_next(schema_lexer_t *lexer, schema_token_t *token);

#ifdef __cplusplus
}
#endif

#endif /* SCHEMA_LEXER_H */
