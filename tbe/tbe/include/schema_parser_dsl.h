#ifndef SCHEMA_PARSER_H
#define SCHEMA_PARSER_H

#include "node_tree.h"
#include "tbe_error.h"

/**
 * @brief Parse a schema text into a mustache-ready Node tree.
 *
 * Uses a re2c-generated lexer and a lemon-generated parser to process
 * the schema definition. The parser replaces @p root's generated
 * "schema", "messages", "composites", "groups", and "enums"
 * children (if any) with fresh parse results while leaving unrelated keys
 * intact.
 *
 * @param text   The NUL-terminated schema text.
 * @param len    Length of the schema text (excluding NUL).
 * @param root   A pre-created NODE_MAP that will receive the generated nodes.
 * @param err    Optional error structure to receive detailed error information (can be NULL).
 * @return 0 on success, -1 on parse error.
 */
int parse_schema(const char *text, size_t len, Node *root, tbe_error_t *err);

#endif /* SCHEMA_PARSER_H */
