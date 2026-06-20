#ifndef TBE_ERROR_H
#define TBE_ERROR_H

#include <stddef.h>

typedef enum {
    TBE_OK = 0,
    TBE_ERR_INVALID_ARGUMENT,
    TBE_ERR_OUT_OF_MEMORY,
    TBE_ERR_LEXER_ERROR,
    TBE_ERR_SYNTAX_ERROR,
    TBE_ERR_SEMANTIC_ERROR,
    TBE_ERR_IO_ERROR
} tbe_error_code_t;

typedef struct {
    tbe_error_code_t code;
    int line;
    int column;
    char message[256];
} tbe_error_t;

/**
 * @brief Get a human-readable description of an error code.
 * @param code The error code.
 * @return A static string describing the error.
 */
const char *tbe_error_string(tbe_error_code_t code);

/**
 * @brief Initialize an error structure.
 * @param err Pointer to the error structure to initialize.
 */
void tbe_error_init(tbe_error_t *err);

/**
 * @brief Set error information.
 * @param err Pointer to the error structure.
 * @param code Error code.
 * @param line Line number (or -1 if unknown).
 * @param column Column number (or -1 if unknown).
 * @param message Error message (will be truncated if too long).
 */
void tbe_error_set(tbe_error_t *err, tbe_error_code_t code, int line, int column, const char *message);

#endif /* TBE_ERROR_H */
