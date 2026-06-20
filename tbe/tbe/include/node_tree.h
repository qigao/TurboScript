#ifndef NODE_TREE_H
#define NODE_TREE_H

#include <stddef.h>

/**
 * @brief Dynamic node tree for Mustache data binding.
 *
 * Provides a simple tree data structure that can represent strings, lists,
 * and maps — exactly what the Mustache template engine needs as input.
 */

/* Schema (DSL) Parser */
typedef enum {
  TURBO_NODE_ROOT,
  TURBO_NODE_STRING,
  TURBO_NODE_LIST,
  TURBO_NODE_MAP,

  /* Compatibility aliases for tbe_compiler */
  NODE_ROOT   = TURBO_NODE_ROOT,
  NODE_STRING = TURBO_NODE_STRING,
  NODE_LIST   = TURBO_NODE_LIST,
  NODE_MAP    = TURBO_NODE_MAP
} turbo_node_type_t;

typedef struct turbo_node_s {
  turbo_node_type_t type;
  const char *name;
  union {
    char *string_val;
    struct {
      struct turbo_node_s **items;
      size_t count;
      size_t cap;
    } list;
    struct {
      struct turbo_node_s **items;
      size_t count;
      size_t cap;
    } map;
  } data;
} turbo_node_t;

/**
 * @brief Dynamic node tree for Mustache data binding.
 *
 * Provides a simple tree data structure that can represent strings, lists,
 * and maps — exactly what the Mustache template engine needs as input.
 */

typedef turbo_node_type_t NodeType;
typedef turbo_node_t Node;

/** Create a string-valued node. Both @p name and @p val are duplicated.
 *  @return Node pointer on success, NULL on allocation failure. */
Node *create_node_string(const char *name, const char *val);

/** Create an empty list node.
 *  @return Node pointer on success, NULL on allocation failure. */
Node *create_node_list(const char *name);

/** Append @p item to @p list (NODE_LIST).
 *  @return 0 on success, -1 on allocation failure. */
int list_add(Node *list, Node *item);

/** Create an empty map node.
 *  @return Node pointer on success, NULL on allocation failure. */
Node *create_node_map(const char *name);

/** Append @p item to @p map  (NODE_MAP).
 *  @return 0 on success, -1 on allocation failure. */
int map_add(Node *map, Node *item);

/** Recursively free a node and all its children. */
void node_free(Node *node);

#endif /* NODE_TREE_H */
