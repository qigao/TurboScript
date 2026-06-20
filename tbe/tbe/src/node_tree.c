#include "node_tree.h"
#include "schema_parser_dsl.h"
#include <stdlib.h>
#include <string.h>

#define NODE_INITIAL_CAPACITY 8
#define NODE_GROWTH_FACTOR 2
#define MAX_RECURSION_DEPTH 1000

Node *create_node_string(const char *name, const char *val) {
  if (!val) return NULL;

  Node *n = (Node *)calloc(1, sizeof(Node));
  if (!n) return NULL;

  n->type = NODE_STRING;
  if (name) {
    n->name = strdup(name);
    if (!n->name) {
      free(n);
      return NULL;
    }
  }

  n->data.string_val = strdup(val);
  if (!n->data.string_val) {
    free((void *)n->name);
    free(n);
    return NULL;
  }

  return n;
}

Node *create_node_list(const char *name) {
  Node *n = (Node *)calloc(1, sizeof(Node));
  if (!n) return NULL;

  n->type = NODE_LIST;
  if (name) {
    n->name = strdup(name);
    if (!n->name) {
      free(n);
      return NULL;
    }
  }
  return n;
}

static int node_add_child(Node *parent, Node *item) {
  Node ***items_ptr;
  size_t *count, *cap;

  if (parent->type == NODE_LIST) {
    items_ptr = &parent->data.list.items;
    count = &parent->data.list.count;
    cap = &parent->data.list.cap;
  } else {
    items_ptr = &parent->data.map.items;
    count = &parent->data.map.count;
    cap = &parent->data.map.cap;
  }

  if (*count == *cap) {
    size_t new_cap = *cap ? *cap * NODE_GROWTH_FACTOR : NODE_INITIAL_CAPACITY;
    Node **new_items = (Node **)realloc(*items_ptr, new_cap * sizeof(Node *));
    if (!new_items) return -1;
    *items_ptr = new_items;
    *cap = new_cap;
  }
  (*items_ptr)[(*count)++] = item;
  return 0;
}

int list_add(Node *list, Node *item) {
  return node_add_child(list, item);
}

Node *create_node_map(const char *name) {
  Node *n = (Node *)calloc(1, sizeof(Node));
  if (!n) return NULL;

  n->type = NODE_MAP;
  if (name) {
    n->name = strdup(name);
    if (!n->name) {
      free(n);
      return NULL;
    }
  }
  return n;
}

int map_add(Node *map, Node *item) {
  return node_add_child(map, item);
}

static void node_free_iterative(Node *root) {
  if (!root) return;
  
  // Use a stack to avoid recursion
  Node **stack = (Node **)malloc(MAX_RECURSION_DEPTH * sizeof(Node *));
  if (!stack) {
    // Fallback to recursive if malloc fails, with depth limit
    node_free(root);
    return;
  }
  
  int stack_top = 0;
  stack[stack_top++] = root;
  
  while (stack_top > 0) {
    Node *node = stack[--stack_top];
    if (!node) continue;
    
    // Add children to stack first (so they get freed first)
    if (node->type == NODE_LIST) {
      for (size_t i = 0; i < node->data.list.count; i++) {
        if (stack_top < MAX_RECURSION_DEPTH - 1) {
          stack[stack_top++] = node->data.list.items[i];
        }
      }
      free(node->data.list.items);
    } else if (node->type == NODE_MAP) {
      for (size_t i = 0; i < node->data.map.count; i++) {
        if (stack_top < MAX_RECURSION_DEPTH - 1) {
          stack[stack_top++] = node->data.map.items[i];
        }
      }
      free(node->data.map.items);
    } else if (node->type == NODE_STRING) {
      free(node->data.string_val);
    }
    
    free((void *)node->name);
    free(node);
  }
  
  free(stack);
}

static void node_free_recursive(Node *node, int depth) {
  if (!node) return;
  if (depth >= MAX_RECURSION_DEPTH) {
    // Switch to iterative approach for deep structures
    node_free_iterative(node);
    return;
  }
  
  switch (node->type) {
  case NODE_STRING:
    free(node->data.string_val);
    break;
  case NODE_LIST:
    for (size_t i = 0; i < node->data.list.count; i++)
      node_free_recursive(node->data.list.items[i], depth + 1);
    free(node->data.list.items);
    break;
  case NODE_MAP:
    for (size_t i = 0; i < node->data.map.count; i++)
      node_free_recursive(node->data.map.items[i], depth + 1);
    free(node->data.map.items);
    break;
  default:
    break;
  }
  free((void *)node->name);
  free(node);
}

void node_free(Node *node) {
  node_free_recursive(node, 0);
}
