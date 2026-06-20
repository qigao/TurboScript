#include "tbe_error.h"
#include <string.h>
#include <stdio.h>

#define TBE_ERROR_MSG_MAX_LEN 255  // Leave room for null terminator
#define TBE_MAX_PARSE_DEPTH 100   // Maximum nesting depth for schema parsing

const char *tbe_error_string(tbe_error_code_t code) {
    switch (code) {
    case TBE_OK:
        return "Success";
    case TBE_ERR_INVALID_ARGUMENT:
        return "Invalid argument";
    case TBE_ERR_OUT_OF_MEMORY:
        return "Out of memory";
    case TBE_ERR_LEXER_ERROR:
        return "Lexer error";
    case TBE_ERR_SYNTAX_ERROR:
        return "Syntax error";
    case TBE_ERR_SEMANTIC_ERROR:
        return "Semantic error";
    case TBE_ERR_IO_ERROR:
        return "I/O error";
    default:
        return "Unknown error";
    }
}

void tbe_error_init(tbe_error_t *err) {
    if (!err) return;
    err->code = TBE_OK;
    err->line = -1;
    err->column = -1;
    err->message[0] = '\0';
}

void tbe_error_set(tbe_error_t *err, tbe_error_code_t code, int line, int column, const char *message) {
    if (!err) return;
    err->code = code;
    err->line = line;
    err->column = column;
    
    if (message && message[0] != '\0') {
        size_t msg_len = strlen(message);
        if (msg_len > TBE_ERROR_MSG_MAX_LEN) {
            msg_len = TBE_ERROR_MSG_MAX_LEN;
        }
        memcpy(err->message, message, msg_len);
        err->message[msg_len] = '\0';
    } else {
        const char *default_msg = tbe_error_string(code);
        size_t default_len = strlen(default_msg);
        if (default_len > TBE_ERROR_MSG_MAX_LEN) {
            default_len = TBE_ERROR_MSG_MAX_LEN;
        }
        memcpy(err->message, default_msg, default_len);
        err->message[default_len] = '\0';
    }
}
