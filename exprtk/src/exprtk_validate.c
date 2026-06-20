#include <exprtk.h>
#include "exprtk_grammar_gen.h"
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

typedef struct validate_ctx_s {
    exprtk_parse_ctx_t *p_ctx;
    exprtk_env_t *env;
    mem_pool_t scratch;
    struct scope_s {
        char **vars;
        size_t count;
        size_t cap;
        struct scope_s *parent;
    } *scope;
} validate_ctx_t;

static void scope_push(validate_ctx_t *ctx) {
    struct scope_s *s = MEM_ALLOC(&ctx->scratch, struct scope_s);
    if (!s) return;
    s->vars = NULL;
    s->count = 0;
    s->cap = 0;
    s->parent = ctx->scope;
    ctx->scope = s;
}

static void scope_pop(validate_ctx_t *ctx) {
    if (!ctx->scope) return;
    struct scope_s *s = ctx->scope;
    ctx->scope = s->parent;
}

static char *safe_strdup(validate_ctx_t *ctx, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *res = (char *)mem_alloc(&ctx->scratch, len + 1);
    if (res) memcpy(res, s, len + 1);
    return res;
}

static void scope_add(validate_ctx_t *ctx, const char *name) {
    if (!ctx || !ctx->scope || !name) return;
    struct scope_s *s = ctx->scope;
    for (size_t i = 0; i < s->count; ++i) {
        if (s->vars[i] && strcmp(s->vars[i], name) == 0) return; // Already in this scope
    }
    if (s->count >= s->cap) {
        size_t new_cap = s->cap == 0 ? 8 : s->cap * 2;
        char **new_vars = MEM_ALLOC_ARRAY(&ctx->scratch, char *, new_cap);
        if (!new_vars) return;
        if (s->vars && s->count > 0) {
            memcpy(new_vars, s->vars, s->count * sizeof(char *));
        }
        s->vars = new_vars;
        s->cap = new_cap;
    }
    s->vars[s->count++] = safe_strdup(ctx, name);
}

static bool scope_has(validate_ctx_t *ctx, const char *name) {
    if (!ctx || !name) return false;
    // 1. Check current scope stack
    struct scope_s *s = ctx->scope;
    while (s) {
        for (size_t i = 0; i < s->count; ++i) {
            if (s->vars[i] && strcmp(s->vars[i], name) == 0) return true;
        }
        s = s->parent;
    }
    // 2. Check environment (pre-defined global vars via hash table)
    if (ctx->env) {
        exprtk_env_t *e = ctx->env;
        while (e) {
            if (exprtk_env_has(e, name)) return true;
            // Also check functions
            exprtk_func_t *f = e->funcs;
            while (f) {
                if (f->name && strcmp(f->name, name) == 0) return true;
                f = f->next;
            }
            e = e->parent;
        }
    }
    return false;
}

static bool is_module_prefix(validate_ctx_t *ctx, const char *name) {
    if (!ctx || !ctx->env || !name) return false;
    /* Check env->modules for a matching module_name */
    exprtk_env_t *e = ctx->env;
    while (e) {
        for (size_t m = 0; m < e->module_count; ++m) {
            if (e->modules[m]->module_name &&
                strcmp(e->modules[m]->module_name, name) == 0)
                return true;
            /* Also check if any entry starts with "name." (e.g. "ta.sma") */
            size_t nlen = strlen(name);
            for (size_t i = 0; i < e->modules[m]->count; ++i) {
                const char *fn_name = e->modules[m]->entries[i].name;
                if (strncmp(fn_name, name, nlen) == 0 && fn_name[nlen] == '.')
                    return true;
            }
        }
        e = e->parent;
    }
    /* Also check if it's a known variable in scope (e.g. bound by import) */
    return scope_has(ctx, name);
}

static void report_error(validate_ctx_t *ctx, exprtk_node_t *node, const char *fmt, ...) {
    if (ctx->p_ctx->error) return;
    ctx->p_ctx->error = 1;
    va_list args;
    va_start(args, fmt);
    char msg[128];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    snprintf(ctx->p_ctx->error_msg, sizeof(ctx->p_ctx->error_msg), 
             "Validation error at line %d, col %d: %s", node->line, node->column, msg);
}

static void validate_node(validate_ctx_t *ctx, exprtk_node_t *node);

static void validate_node(validate_ctx_t *ctx, exprtk_node_t *node) {
    if (!node || ctx->p_ctx->error) return;

    switch (node->type) {
        case EXPRTK_NODE_VARIABLE:
            if (!scope_has(ctx, node->data.variable.name)) {
                // report_error(ctx, node, "Undefined variable '%s'", node->data.variable.name);
                // We'll skip for now to avoid false positives on dynamic registrations or external bindings
            }
            break;

        case EXPRTK_NODE_ASSIGNMENT:
        case EXPRTK_NODE_CONSTANT_DECL:
            validate_node(ctx, node->data.assignment.value);
            scope_add(ctx, node->data.assignment.name);
            break;

        case EXPRTK_NODE_BINARY_OP:
            validate_node(ctx, node->data.binary.left);
            validate_node(ctx, node->data.binary.right);
            // Simple Type Checking
            if (node->data.binary.op == exprtk_TOKEN_PLUS || 
                node->data.binary.op == exprtk_TOKEN_MINUS ||
                node->data.binary.op == exprtk_TOKEN_MULTIPLY ||
                node->data.binary.op == exprtk_TOKEN_DIVIDE) {
                // We don't have full type inference yet, but we can catch obvious literals
                if ((node->data.binary.left && node->data.binary.left->type == EXPRTK_NODE_MAP_LITERAL) ||
                    (node->data.binary.right && node->data.binary.right->type == EXPRTK_NODE_MAP_LITERAL)) {
                    report_error(ctx, node, "Invalid operation on Map type");
                }
            }
            break;

        case EXPRTK_NODE_FUNCTION_EXPRESSION:
        case EXPRTK_NODE_FUNCTION_DEFINITION: {
            scope_push(ctx);
            for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
                exprtk_node_t *arg = node->data.func_def.arg_params[i];
                if (arg->type == EXPRTK_NODE_VARIABLE) {
                    const char *arg_name = arg->data.variable.name;
                    // Check for duplicate parameters
                    struct scope_s *s = ctx->scope;
                    for (size_t j = 0; j < s->count; ++j) {
                        if (strcmp(s->vars[j], arg_name) == 0) {
                            report_error(ctx, arg, "Duplicate parameter '%s'", arg_name);
                            break;
                        }
                    }
                    scope_add(ctx, arg_name);
                }
            }
            validate_node(ctx, node->data.func_def.body);
            scope_pop(ctx);
            // Add function name to outer scope if it's a named function
            if (node->data.func_def.name) scope_add(ctx, node->data.func_def.name);
            break;
        }

        case EXPRTK_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                validate_node(ctx, node->data.block.statements[i]);
            }
            break;

        case EXPRTK_NODE_IF:
            validate_node(ctx, node->data.if_stmt.condition);
            validate_node(ctx, node->data.if_stmt.if_branch);
            validate_node(ctx, node->data.if_stmt.else_branch);
            break;

        case EXPRTK_NODE_WHILE:
        case EXPRTK_NODE_DO_WHILE:
            validate_node(ctx, node->data.while_loop.condition);
            validate_node(ctx, node->data.while_loop.body);
            break;

        case EXPRTK_NODE_FOR:
            scope_push(ctx);
            validate_node(ctx, node->data.for_loop.init);
            validate_node(ctx, node->data.for_loop.condition);
            validate_node(ctx, node->data.for_loop.post);
            validate_node(ctx, node->data.for_loop.body);
            scope_pop(ctx);
            break;

        case EXPRTK_NODE_FOR_IN:
            scope_push(ctx);
            validate_node(ctx, node->data.for_in.collection);
            scope_add(ctx, node->data.for_in.var_name);
            validate_node(ctx, node->data.for_in.body);
            scope_pop(ctx);
            break;

        case EXPRTK_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                validate_node(ctx, node->data.function.args[i]);
            }
            
            // Handle import calls: add the module name to scope
            if (node->data.function.name && strcmp(node->data.function.name, "import") == 0 && node->data.function.arg_count == 1) {
                exprtk_node_t *arg = node->data.function.args[0];
                if (arg->type == EXPRTK_NODE_STRING) {
                    // Add the import name itself as a scope variable
                    char mod_name[64];
                    size_t mod_len = arg->data.string.value.len < 63 ? arg->data.string.value.len : 63;
                    memcpy(mod_name, arg->data.string.value.data, mod_len);
                    mod_name[mod_len] = '\0';
                    scope_add(ctx, mod_name);

                    // Scan env modules for any function prefixed with known namespaces
                    // to add those namespace prefixes to scope
                    if (ctx->env) {
                        exprtk_env_t *e = ctx->env;
                        while (e) {
                            for (size_t m = 0; m < e->module_count; ++m) {
                                for (size_t fi = 0; fi < e->modules[m]->count; ++fi) {
                                    const char *fn = e->modules[m]->entries[fi].name;
                                    const char *dot = strchr(fn, '.');
                                    if (dot) {
                                        char prefix[64];
                                        size_t plen = (size_t)(dot - fn);
                                        if (plen < 64) {
                                            memcpy(prefix, fn, plen);
                                            prefix[plen] = '\0';
                                            scope_add(ctx, prefix);
                                        }
                                    }
                                }
                            }
                            e = e->parent;
                        }
                    }
                }
            }

            // Check if function exists in env or registry
            if (node->data.function.name) {
                if (!exprtk_find_builtin(node->data.function.name, ctx->env) && 
                    !exprtk_registry_find(node->data.function.name) &&
                    strcmp(node->data.function.name, "import") != 0 &&
                    strcmp(node->data.function.name, "print") != 0 &&
                    strcmp(node->data.function.name, "println") != 0) {
                    // Search in local function definitions or script funcs?
                    // For now, call it "warning" or just ignore if we don't have full symbol table
                    // We'll skip for now to avoid false positives on dynamic registrations
                }
            }
            break;

        case EXPRTK_NODE_MEMBER_CALL:
            // Skip variable check if the object is a known module prefix
            if (node->data.member_call.object && node->data.member_call.object->type == EXPRTK_NODE_VARIABLE) {
                const char *obj_name = node->data.member_call.object->data.variable.name;
                if (!is_module_prefix(ctx, obj_name)) {
                    validate_node(ctx, node->data.member_call.object);
                }
            } else {
                validate_node(ctx, node->data.member_call.object);
            }
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
                validate_node(ctx, node->data.member_call.args[i]);
            }
            break;

        case EXPRTK_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                validate_node(ctx, node->data.vector.elements[i]);
            }
            break;

        case EXPRTK_NODE_MAP_LITERAL:
            for (size_t i = 0; i < node->data.map_literal.count; ++i) {
                validate_node(ctx, node->data.map_literal.values[i]);
            }
            break;

        default:
            break;
    }
}

int exprtk_validate(exprtk_node_t *root, exprtk_env_t *env, char *error_msg, size_t msg_len) {
    if (!root) return 0;
    
    exprtk_parse_ctx_t p_ctx;
    memset(&p_ctx, 0, sizeof(p_ctx));
    
    validate_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.p_ctx = &p_ctx;
    ctx.env = env;
    ctx.scope = NULL;
    if (mem_init(&ctx.scratch, 4096) != 0) {
        if (error_msg && msg_len > 0) {
            snprintf(error_msg, msg_len, "Validation error: out of memory");
        }
        return 1;
    }
    
    scope_push(&ctx);
    validate_node(&ctx, root);
    scope_pop(&ctx);
    
    if (p_ctx.error) {
        if (error_msg && msg_len > 0) {
            size_t copy_len = strlen(p_ctx.error_msg);
            if (copy_len >= msg_len) copy_len = msg_len - 1;
            memcpy(error_msg, p_ctx.error_msg, copy_len);
            error_msg[copy_len] = '\0';
        }
        mem_destroy(&ctx.scratch);
        return 1;
    }
    mem_destroy(&ctx.scratch);
    return 0;
}
