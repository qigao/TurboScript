/**
 * @file exprtk_lexer.h
 * @brief exprtk Lexer Definitions
 */

#ifndef exprtk_LEXER_H
#define exprtk_LEXER_H

#include <stddef.h>
#include "exprtk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *input;
    const char *cursor;
    const char *limit;
    int line;
    int column;
    char error[256];
} exprtk_lexer_t;

void exprtk_lexer_init(exprtk_lexer_t *lexer, const char *input, size_t length);
int  exprtk_lexer_next(exprtk_lexer_t *lexer, exprtk_token_t *token);

#ifdef __cplusplus
}
#endif

#endif /* exprtk_LEXER_H */

