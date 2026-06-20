// re2c $INPUT -o $OUTPUT
/**
 * @file schema_lexer.re
 * @brief Schema Lexer using re2c for tbe_compiler
 *
 * Tokenizes schema text into: SCHEMA, MESSAGE, COMPOSITE, GROUP,
 * IDENT, LBRACE, RBRACE, SEMI,
 * LPAREN, RPAREN, LBRACKET, RBRACKET, LT, GT, COMMA
 *
 * Build: re2c -o schema_lexer_gen.c schema_lexer.re
 */

#include "schema_lexer.h"
#include "schema_grammar_gen.h"
#include <string.h>

int schema_lex(schema_lexer_t *lexer, schema_token_t *token) {
    if (!lexer || !token) return -1;

    const char *YYCURSOR = lexer->cursor;
    const char *YYMARKER;
    const char *YYLIMIT = lexer->limit;
    const char *token_start;

    token->value = NULL;
    token->length = 0;
    token->line = lexer->line;
    token->column = (int)(lexer->cursor - lexer->line_start) + 1;

lex_start:
    token_start = YYCURSOR;
    token->line = lexer->line;
    token->column = (int)(token_start - lexer->line_start) + 1;

    /*!re2c
        re2c:define:YYCTYPE = "char";
        re2c:yyfill:enable = 0;
        re2c:eof = 0;

        ws      = [ \t\r]+;
        newline = "\r\n" | "\r" | "\n";
        comment = "//" [^\r\n\x00]*;
        ident   = [a-zA-Z_][a-zA-Z0-9_]*;

        // End of input
        $ {
            token->type = 0;
            lexer->cursor = YYCURSOR;
            return 0;
        }

        // Skip whitespace
        ws {
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        // Newline (skip, but track line number)
        newline {
            lexer->line++;
            lexer->cursor = YYCURSOR;
            lexer->line_start = YYCURSOR;
            goto lex_start;
        }

        // Comment (skip)
        comment {
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        // Keywords
        "message" {
            token->type = SCHEMA_TOKEN_MESSAGE;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "composite" {
            token->type = SCHEMA_TOKEN_COMPOSITE;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "group" {
            token->type = SCHEMA_TOKEN_GROUP;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "schema" {
            token->type = SCHEMA_TOKEN_SCHEMA;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "enum" {
            token->type = SCHEMA_TOKEN_ENUM;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "flags" {
            token->type = SCHEMA_TOKEN_FLAGS;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "union" {
            token->type = SCHEMA_TOKEN_UNION;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "required" {
            token->type = SCHEMA_TOKEN_REQUIRED;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "optional" {
            token->type = SCHEMA_TOKEN_OPTIONAL;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "default" {
            token->type = SCHEMA_TOKEN_DEFAULT;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        // Literals
        "0x" [0-9a-fA-F]+ {
            token->type = SCHEMA_TOKEN_NUMBER;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        [0-9]+ {
            token->type = SCHEMA_TOKEN_NUMBER;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        // String literals for default values
        "\"" [^\"\r\n\x00]* "\"" {
            token->type = SCHEMA_TOKEN_STRING;
            token->value = token_start + 1;  // Skip opening quote
            token->length = (size_t)(YYCURSOR - token_start - 2);  // Exclude quotes
            lexer->cursor = YYCURSOR;
            return 1;
        }

        // Boolean literals
        "true" {
            token->type = SCHEMA_TOKEN_TRUE;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "false" {
            token->type = SCHEMA_TOKEN_FALSE;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        // Punctuation
        "=" {
            token->type = SCHEMA_TOKEN_EQUALS;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "{" {

            token->type = SCHEMA_TOKEN_LBRACE;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "}" {
            token->type = SCHEMA_TOKEN_RBRACE;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        ";" {
            token->type = SCHEMA_TOKEN_SEMI;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "(" {
            token->type = SCHEMA_TOKEN_LPAREN;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        ")" {
            token->type = SCHEMA_TOKEN_RPAREN;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "[" {
            token->type = SCHEMA_TOKEN_LBRACKET;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "]" {
            token->type = SCHEMA_TOKEN_RBRACKET;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "<" {
            token->type = SCHEMA_TOKEN_LT;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        ">" {
            token->type = SCHEMA_TOKEN_GT;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        "," {
            token->type = SCHEMA_TOKEN_COMMA;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return 1;
        }

        // Identifier (must come after keyword rules)
        ident {
            token->type = SCHEMA_TOKEN_IDENT;
            token->value = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return 1;
        }

        // Any other character is invalid
        * {
            token->type = 0;
            token->value = token_start;
            token->length = 1;
            lexer->cursor = YYCURSOR;
            return -1;
        }
    */
}

void schema_lexer_init(schema_lexer_t *lexer, const char *input, size_t length) {
    if (!lexer) return;
    lexer->input = input;
    lexer->cursor = input;
    lexer->limit = input + length;
    lexer->line_start = input;
    lexer->line = 1;
    lexer->column = 1;
}

int schema_lexer_next(schema_lexer_t *lexer, schema_token_t *token) {
    return schema_lex(lexer, token);
}
