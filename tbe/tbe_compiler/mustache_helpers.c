#include "mustache_helpers.h"
#include "node_tree.h"
#include <stdio.h>
#include <string.h>

static void *get_root(void *provider_data) {
    return provider_data;
}

static int dump_node(void *node, int (*out_fn)(const char *, size_t, void *),
                     void *renderer_data, void *provider_data) {
    (void)provider_data;
    Node *n = (Node *)node;
    if (n && n->type == NODE_STRING && n->data.string_val) {
        return out_fn(n->data.string_val, strlen(n->data.string_val), renderer_data);
    }
    return 0;
}

static void *get_child_by_name(void *node, const char *name, size_t size,
                               void *provider_data) {
    (void)provider_data;
    Node *n = (Node *)node;
    if (!n || size == 0) return NULL;

    const char *dot = memchr(name, '.', size);
    if (dot) {
        size_t head_size = (size_t)(dot - name);
        void *child = get_child_by_name(node, name, head_size, provider_data);
        if (!child) return NULL;
        return get_child_by_name(child, dot + 1, size - head_size - 1, provider_data);
    }

    if (n->type == NODE_MAP) {
        for (size_t i = 0; i < n->data.map.count; i++) {
            Node *child = n->data.map.items[i];
            if (child->name && strlen(child->name) == size &&
                strncmp(child->name, name, size) == 0) {
                return child;
            }
        }
    }
    return NULL;
}

static void *get_child_by_index(void *node, unsigned index,
                                void *provider_data) {
    (void)provider_data;
    Node *n = (Node *)node;
    if (!n) return NULL;
    if (n->type == NODE_LIST) {
        if (index < n->data.list.count) return n->data.list.items[index];
    } else if (n->type == NODE_MAP) {
        if (index < n->data.map.count) return n->data.map.items[index];
    } else {
        if (index == 0) return n;
    }
    return NULL;
}

static MUSTACHE_TEMPLATE *get_partial(const char *name, size_t size,
                                      void *provider_data) {
    (void)name; (void)size; (void)provider_data;
    return NULL; // No partials
}

static int out_verbatim(const char *output, size_t size, void *renderer_data) {
    FILE *out_file = renderer_data ? (FILE *)renderer_data : stdout;
    fwrite(output, 1, size, out_file);
    return 0;
}

MUSTACHE_DATAPROVIDER mustache_helpers_provider(void) {
    MUSTACHE_DATAPROVIDER provider = {
        .get_root          = get_root,
        .dump              = dump_node,
        .get_child_by_name = get_child_by_name,
        .get_child_by_index = get_child_by_index,
        .get_partial       = get_partial
    };
    return provider;
}

MUSTACHE_RENDERER mustache_helpers_renderer(void) {
    MUSTACHE_RENDERER renderer = {
        .out_verbatim = out_verbatim,
        .out_escaped  = out_verbatim
    };
    return renderer;
}
