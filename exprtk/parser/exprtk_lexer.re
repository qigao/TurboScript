// re2c $INPUT -o $OUTPUT
/**
 * @file exprtk_lexer.re
 * @brief exprtk Lexer using re2c
 */

#include "exprtk_lexer.h"
#include "exprtk_grammar_gen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static double parse_number(const char *start, const char *end) {
    char buf[64];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return strtod(buf, NULL);
}

static int regex_can_start_after(int token_type) {
    switch (token_type) {
        case 0:
        case exprtk_TOKEN_EQUAL:
        case exprtk_TOKEN_ASSIGN_ADD:
        case exprtk_TOKEN_ASSIGN_SUB:
        case exprtk_TOKEN_ASSIGN_MUL:
        case exprtk_TOKEN_ASSIGN_DIV:
        case exprtk_TOKEN_LPAREN:
        case exprtk_TOKEN_LBRACKET:
        case exprtk_TOKEN_LBRACE:
        case exprtk_TOKEN_COMMA:
        case exprtk_TOKEN_COLON:
        case exprtk_TOKEN_SEMICOLON:
        case exprtk_TOKEN_RETURN:
        case exprtk_TOKEN_THROW:
        case exprtk_TOKEN_CASE:
        case exprtk_TOKEN_QUESTION:
        case exprtk_TOKEN_ARROW:
        case exprtk_TOKEN_PLUS:
        case exprtk_TOKEN_MINUS:
        case exprtk_TOKEN_MULTIPLY:
        case exprtk_TOKEN_DIVIDE:
        case exprtk_TOKEN_MOD:
        case exprtk_TOKEN_POWER:
        case exprtk_TOKEN_EQ:
        case exprtk_TOKEN_NE:
        case exprtk_TOKEN_LT:
        case exprtk_TOKEN_LE:
        case exprtk_TOKEN_GT:
        case exprtk_TOKEN_GE:
        case exprtk_TOKEN_AND:
        case exprtk_TOKEN_OR:
        case exprtk_TOKEN_NOT:
        case exprtk_TOKEN_PIPE:
            return 1;
        default:
            return 0;
    }
}

static int scan_regex_literal(const char *start, const char *limit, const char **end) {
    const char *p = start + 1;
    int escaped = 0;
    int in_class = 0;

    while (p < limit) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r') return -1;
        if (escaped) {
            escaped = 0;
            ++p;
            continue;
        }
        if (c == '\\') {
            escaped = 1;
            ++p;
            continue;
        }
        if (c == '[') {
            in_class = 1;
            ++p;
            continue;
        }
        if (c == ']' && in_class) {
            in_class = 0;
            ++p;
            continue;
        }
        if (c == '/' && !in_class) {
            ++p;
            while (p < limit && isalpha((unsigned char)*p)) ++p;
            *end = p;
            return 1;
        }
        ++p;
    }
    return -1;
}

static void lexer_note_token(exprtk_lexer_t *lexer, int token_type) {
    lexer->last_token = token_type;
}

int exprtk_lexer_next(exprtk_lexer_t *lexer, exprtk_token_t *token) {
    if (!lexer || !token) return -1;

    const char *YYCURSOR = lexer->cursor;
    const char *YYMARKER;
    const char *YYLIMIT = lexer->limit;
    const char *token_start;

    token->type = 0;

    #define RET(t) do { \
        token->type = t; \
        token->start = token_start; \
        token->length = (size_t)(YYCURSOR - token_start); \
        lexer->cursor = YYCURSOR; \
        lexer->column += (int)(YYCURSOR - token_start); \
        lexer_note_token(lexer, t); \
        return t; \
    } while(0)

lex_start:
    token_start = YYCURSOR;
    token->line = lexer->line;
    token->column = lexer->column;

    /*!re2c
        re2c:define:YYCTYPE = "unsigned char";
        re2c:yyfill:enable = 0;
        re2c:eof = 0;

        digit   = [0-9];
        int     = "0" | [1-9] digit*;
        frac    = "." digit+;
        exp     = [eE] [+-]? digit+;
        number  = int frac? exp?;
        
        ident   = [a-zA-Z_] [a-zA-Z0-9_]*;

        // EOF
        $ {
            token->type = 0;
            token->line = lexer->line;
            token->column = lexer->column;
            lexer->cursor = YYCURSOR;
            return 0;
        }

        // Operators
        "+" { RET(exprtk_TOKEN_PLUS); }
        "-" { RET(exprtk_TOKEN_MINUS); }
        "*" { RET(exprtk_TOKEN_MULTIPLY); }
        "/" {
            if (regex_can_start_after(lexer->last_token)) {
                const char *regex_end = NULL;
                int regex_scan = scan_regex_literal(token_start, YYLIMIT, &regex_end);
                if (regex_scan > 0) {
                    YYCURSOR = regex_end;
                    RET(exprtk_TOKEN_REGEX);
                }
                if (regex_scan < 0) {
                    snprintf(lexer->error, sizeof(lexer->error),
                             "Unterminated regex literal at line %d", lexer->line);
                    return -1;
                }
            }
            RET(exprtk_TOKEN_DIVIDE);
        }
        "%" { RET(exprtk_TOKEN_MOD); }
        "^" { RET(exprtk_TOKEN_POWER); }
        "..." { RET(exprtk_TOKEN_SPREAD); }
        ".." { RET(exprtk_TOKEN_DOTDOT); }
        "."  { RET(exprtk_TOKEN_DOT); }
        "?." { RET(exprtk_TOKEN_QUESTION_DOT); }
        "?"  { RET(exprtk_TOKEN_QUESTION); }

        // Comparison
        "==" { RET(exprtk_TOKEN_EQ); }
        "=>" { RET(exprtk_TOKEN_ARROW); }
        "="  { RET(exprtk_TOKEN_EQUAL); }
        "!=" { RET(exprtk_TOKEN_NE); }
        "<>" { RET(exprtk_TOKEN_NE); }
        "<"  { RET(exprtk_TOKEN_LT); }
        "<=" { RET(exprtk_TOKEN_LE); }
        ">"  { RET(exprtk_TOKEN_GT); }
        ">=" { RET(exprtk_TOKEN_GE); }

        // Assignment

        "+=" { RET(exprtk_TOKEN_ASSIGN_ADD); }
        "-=" { RET(exprtk_TOKEN_ASSIGN_SUB); }
        "*=" { RET(exprtk_TOKEN_ASSIGN_MUL); }
        "/=" { RET(exprtk_TOKEN_ASSIGN_DIV); }

        // Logic
        "and" { RET(exprtk_TOKEN_AND); }
        "or"  { RET(exprtk_TOKEN_OR); }
        "not" { RET(exprtk_TOKEN_NOT); }
        "|>"  { RET(exprtk_TOKEN_PIPE); }
        "&&"  { RET(exprtk_TOKEN_AND); }
        "||"  { RET(exprtk_TOKEN_OR); }
        "!"   { RET(exprtk_TOKEN_NOT); }

        // Control Flow
        "if"       { RET(exprtk_TOKEN_IF); }
        "elif"     { RET(exprtk_TOKEN_ELIF); }
        "else"     { RET(exprtk_TOKEN_ELSE); }
        "while"    { RET(exprtk_TOKEN_WHILE); }
        "do"       { RET(exprtk_TOKEN_DO); }
        "for"      { RET(exprtk_TOKEN_FOR); }
        "break"    { RET(exprtk_TOKEN_BREAK); }
        "continue" { RET(exprtk_TOKEN_CONTINUE); }
        "return"   { RET(exprtk_TOKEN_RETURN); }
        "var"      { RET(exprtk_TOKEN_VAR); }
        "let"      { RET(exprtk_TOKEN_VAR); }
        "const"    { RET(exprtk_TOKEN_CONST); }

        // OOP Keywords
        "class"       { RET(exprtk_TOKEN_CLASS); }
        "constructor" { RET(exprtk_TOKEN_CONSTRUCTOR); }
        "this"        { RET(exprtk_TOKEN_THIS); }
        "static"      { RET(exprtk_TOKEN_STATIC); }
        "extends"     { RET(exprtk_TOKEN_EXTENDS); }
        "implements"  { RET(exprtk_TOKEN_IMPLEMENTS); }
        "interface"   { RET(exprtk_TOKEN_INTERFACE); }
        "super"       { RET(exprtk_TOKEN_SUPER); }
        "new"         { RET(exprtk_TOKEN_NEW); }
        "instanceof"  { RET(exprtk_TOKEN_INSTANCEOF); }
        "abstract"    { RET(exprtk_TOKEN_ABSTRACT); }
        "override"    { RET(exprtk_TOKEN_OVERRIDE); }
        "final"       { RET(exprtk_TOKEN_FINAL); }
        "public"      { RET(exprtk_TOKEN_PUBLIC); }
        "protected"   { RET(exprtk_TOKEN_PROTECTED); }
        "private"     { RET(exprtk_TOKEN_PRIVATE); }
        "async"       { RET(exprtk_TOKEN_ASYNC); }
        "await"       { RET(exprtk_TOKEN_AWAIT); }
        // TODO: 协程语法暂时禁用
        // "function*" { RET(exprtk_TOKEN_FUNC_GENERATOR); }
        // "func*"    { RET(exprtk_TOKEN_FUNC_GENERATOR); }
        "function" { RET(exprtk_TOKEN_FUNC); }
        "func"     { RET(exprtk_TOKEN_FUNC); }
        "map"      { RET(exprtk_TOKEN_MAP); }
        "struct"   { RET(exprtk_TOKEN_MAP); }
        "in"       { RET(exprtk_TOKEN_IN); }
        "null"     { RET(exprtk_TOKEN_NULL); }
        "nil"      { RET(exprtk_TOKEN_NULL); }
        "switch"   { RET(exprtk_TOKEN_SWITCH); }
        "case"     { RET(exprtk_TOKEN_CASE); }
        "default"  { RET(exprtk_TOKEN_DEFAULT); }
        "try"      { RET(exprtk_TOKEN_TRY); }
        "catch"    { RET(exprtk_TOKEN_CATCH); }
        "throw"    { RET(exprtk_TOKEN_THROW); }
        // TODO: 协程语法暂时禁用
        // "yield"    { RET(exprtk_TOKEN_YIELD); }

        // Structure
        "{" { RET(exprtk_TOKEN_LBRACE); }
        "}" { RET(exprtk_TOKEN_RBRACE); }
        ";" { RET(exprtk_TOKEN_SEMICOLON); }
        ":" { RET(exprtk_TOKEN_COLON); }
        "..." { RET(exprtk_TOKEN_SPREAD); }

        // Punctuation
        "(" { RET(exprtk_TOKEN_LPAREN); }
        ")" { RET(exprtk_TOKEN_RPAREN); }
        "[" { RET(exprtk_TOKEN_LBRACKET); }
        "]" { RET(exprtk_TOKEN_RBRACKET); }
        "," { RET(exprtk_TOKEN_COMMA); }

        "\"" ([^"\\] | "\\" .)* "\"" {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            RET(exprtk_TOKEN_STRING);
        }

        "`" ([^`\\] | "\\" .)* "`" {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            RET(exprtk_TOKEN_TEMPLATE);
        }

        "//" [^\n]* {
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        "/*" {
             while (YYCURSOR < YYLIMIT) {
                 if (*YYCURSOR == '\n') { lexer->line++; lexer->column = 1; }
                 else { lexer->column++; }
                 if (*YYCURSOR == '*' && YYCURSOR + 1 < YYLIMIT && YYCURSOR[1] == '/') {
                     YYCURSOR += 2; lexer->column += 2; break;
                 }
                 YYCURSOR++;
             }
             lexer->cursor = YYCURSOR;
             goto lex_start;
        }

        "\n" {
            lexer->line++;
            lexer->column = 1;
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        [ \t\r]+ {
            lexer->column += (int)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        number {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            token->line = lexer->line;
            token->column = lexer->column;
            
            // Check if it's an integer (no '.' or 'e'/'E')
            int is_integer = 1;
            for (const char *p = token_start; p < YYCURSOR; ++p) {
                if (*p == '.' || *p == 'e' || *p == 'E') {
                    is_integer = 0;
                    break;
                }
            }
            
            if (is_integer) {
                token->type = exprtk_TOKEN_INTEGER;
                char buf[32];
                size_t len = token->length;
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, token_start, len);
                buf[len] = '\0';
                token->num_value = (double)strtoll(buf, NULL, 10);
            } else {
                token->type = exprtk_TOKEN_NUMBER;
                token->num_value = parse_number(token_start, YYCURSOR);
            }
            
            lexer->column += (int)token->length;
            lexer->cursor = YYCURSOR;
            lexer_note_token(lexer, token->type);
            return token->type;
        }

        ident {
            token->type = exprtk_TOKEN_VARIABLE;
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            token->line = lexer->line;
            token->column = lexer->column;
            lexer->column += (int)token->length;
            lexer->cursor = YYCURSOR;
            lexer_note_token(lexer, exprtk_TOKEN_VARIABLE);
            return exprtk_TOKEN_VARIABLE;
        }

        * {
            // Unexpected
            // token->type = exprtk_TOKEN_ERROR;
            // lexer->cursor = YYCURSOR;
            // return -1;
            // Skip unknown chars for now or error?
            // Let's error
            return -1;
        }
    */
}

void exprtk_lexer_init(exprtk_lexer_t *lexer, const char *input, size_t length) {
    if (!lexer) return;
    lexer->input = input;
    lexer->cursor = input;
    lexer->limit = input + length;
    lexer->line = 1;
    lexer->column = 1;
    lexer->last_token = 0;
    lexer->error[0] = '\0';
}

