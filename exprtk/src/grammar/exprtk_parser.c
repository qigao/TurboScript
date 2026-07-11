/**
 * @file exprtk_parser.c
 * @brief DSL Grammar: Lexer, Parser, and AST nodes
 * Separated from monolithic exprtk_core.c.
 */

#include "exprtk_internal.h"
#include "exprtk_lexer.h"
#include "exprtk_grammar_gen.h"

void exprtkParse(void *yyp, int yymajor, exprtk_token_t yyminor, exprtk_parse_ctx_t *ctx);
void *exprtkParseAlloc(void *(*mallocProc)(size_t));
void exprtkParseFree(void *p, void (*freeProc)(void*));
void exprtkParseTrace(FILE *TraceFILE, char *zTracePrompt);


// Deep copy an AST node and its children into a new arena
exprtk_node_t *exprtk_node_copy(const exprtk_node_t *src, mem_pool_t *dest_arena) {
    if (!src) return NULL;
    exprtk_node_t *dst = (exprtk_node_t*)mem_alloc(dest_arena, sizeof(exprtk_node_t));
    if (!dst) return NULL;
    
    memcpy(dst, src, sizeof(exprtk_node_t));
    dst->arena = dest_arena;
    
    switch (src->type) {
        case EXPRTK_NODE_NUMBER:
        case EXPRTK_NODE_VARIABLE:
            if (src->type == EXPRTK_NODE_VARIABLE && src->data.variable.name) {
                dst->data.variable.name = mem_strdup(dest_arena, src->data.variable.name);
            }
            break;
            
        case EXPRTK_NODE_BINARY_OP:
            if (src->data.binary.left) dst->data.binary.left = exprtk_node_copy(src->data.binary.left, dest_arena);
            dst->data.binary.right = exprtk_node_copy(src->data.binary.right, dest_arena);
            break;
            
        case EXPRTK_NODE_FUNCTION_CALL:
            if (src->data.function.name) dst->data.function.name = mem_strdup(dest_arena, src->data.function.name);
            if (src->data.function.arg_count > 0) {
                dst->data.function.args = (exprtk_node_t**)mem_alloc(dest_arena, src->data.function.arg_count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.function.arg_count; ++i) {
                    dst->data.function.args[i] = exprtk_node_copy(src->data.function.args[i], dest_arena);
                }
            }
            break;
            
        case EXPRTK_NODE_CONSTANT_DECL:
        case EXPRTK_NODE_ASSIGNMENT:
            if (src->data.assignment.name) dst->data.assignment.name = mem_strdup(dest_arena, src->data.assignment.name);
            dst->data.assignment.value = exprtk_node_copy(src->data.assignment.value, dest_arena);
            break;
            
        case EXPRTK_NODE_IF:
            dst->data.if_stmt.condition = exprtk_node_copy(src->data.if_stmt.condition, dest_arena);
            dst->data.if_stmt.if_branch = exprtk_node_copy(src->data.if_stmt.if_branch, dest_arena);
            if (src->data.if_stmt.else_branch) dst->data.if_stmt.else_branch = exprtk_node_copy(src->data.if_stmt.else_branch, dest_arena);
            break;
            
        case EXPRTK_NODE_WHILE:
            dst->data.while_loop.condition = exprtk_node_copy(src->data.while_loop.condition, dest_arena);
            dst->data.while_loop.body = exprtk_node_copy(src->data.while_loop.body, dest_arena);
            break;
            
        case EXPRTK_NODE_FOR:
            if (src->data.for_loop.init) dst->data.for_loop.init = exprtk_node_copy(src->data.for_loop.init, dest_arena);
            if (src->data.for_loop.condition) dst->data.for_loop.condition = exprtk_node_copy(src->data.for_loop.condition, dest_arena);
            if (src->data.for_loop.post) dst->data.for_loop.post = exprtk_node_copy(src->data.for_loop.post, dest_arena);
            dst->data.for_loop.body = exprtk_node_copy(src->data.for_loop.body, dest_arena);
            break;
            
        case EXPRTK_NODE_BLOCK:
            if (src->data.block.count > 0) {
                dst->data.block.statements = (exprtk_node_t**)mem_alloc(dest_arena, src->data.block.count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.block.count; ++i) {
                    dst->data.block.statements[i] = exprtk_node_copy(src->data.block.statements[i], dest_arena);
                }
            }
            break;
            
        case EXPRTK_NODE_FLOW:
            if (src->data.flow.value) dst->data.flow.value = exprtk_node_copy(src->data.flow.value, dest_arena);
            break;
            
        case EXPRTK_NODE_STRING:
            if (src->data.string.value.data) {
                char *buf = (char*)mem_alloc(dest_arena, src->data.string.value.len + 1);
                memcpy(buf, src->data.string.value.data, src->data.string.value.len);
                buf[src->data.string.value.len] = '\0';
                dst->data.string.value.data = buf;
            }
            break;
            
        case EXPRTK_NODE_VECTOR:
            if (src->data.vector.count > 0) {
                dst->data.vector.elements = (exprtk_node_t**)mem_alloc(dest_arena, src->data.vector.count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.vector.count; ++i) {
                    dst->data.vector.elements[i] = exprtk_node_copy(src->data.vector.elements[i], dest_arena);
                }
            }
            break;
            
        case EXPRTK_NODE_INDEX:
            dst->data.index_access.array = exprtk_node_copy(src->data.index_access.array, dest_arena);
            dst->data.index_access.index = exprtk_node_copy(src->data.index_access.index, dest_arena);
            break;
            
        case EXPRTK_NODE_SLICE:
            dst->data.slice.array = exprtk_node_copy(src->data.slice.array, dest_arena);
            if (src->data.slice.start) dst->data.slice.start = exprtk_node_copy(src->data.slice.start, dest_arena);
            if (src->data.slice.end) dst->data.slice.end = exprtk_node_copy(src->data.slice.end, dest_arena);
            break;
            
        case EXPRTK_NODE_FUNCTION_EXPRESSION:
        case EXPRTK_NODE_FUNCTION_DEFINITION:
            if (src->data.func_def.name) dst->data.func_def.name = mem_strdup(dest_arena, src->data.func_def.name);
            if (src->data.func_def.arg_count > 0) {
                dst->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(dest_arena, src->data.func_def.arg_count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.func_def.arg_count; ++i) {
                    dst->data.func_def.arg_params[i] = exprtk_node_copy(src->data.func_def.arg_params[i], dest_arena);
                }
            }
            dst->data.func_def.body = exprtk_node_copy(src->data.func_def.body, dest_arena);
            break;

        case EXPRTK_NODE_MEMBER_CALL:
            dst->data.member_call.object = exprtk_node_copy(src->data.member_call.object, dest_arena);
            if (src->data.member_call.method)
                dst->data.member_call.method = mem_strdup(dest_arena, src->data.member_call.method);
            if (src->data.member_call.arg_count > 0) {
                dst->data.member_call.args = (exprtk_node_t**)mem_alloc(dest_arena,
                    src->data.member_call.arg_count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.member_call.arg_count; ++i)
                    dst->data.member_call.args[i] = exprtk_node_copy(src->data.member_call.args[i], dest_arena);
            }
            break;

        case EXPRTK_NODE_INSTANCEOF:
            dst->data.instanceof_expr.object =
                exprtk_node_copy(src->data.instanceof_expr.object, dest_arena);
            if (src->data.instanceof_expr.class_name)
                dst->data.instanceof_expr.class_name =
                    mem_strdup(dest_arena, src->data.instanceof_expr.class_name);
            dst->data.instanceof_expr.class_expr =
                exprtk_node_copy(src->data.instanceof_expr.class_expr, dest_arena);
            break;

        case EXPRTK_NODE_SPREAD:
            dst->data.spread.child = exprtk_node_copy(src->data.spread.child, dest_arena);
            break;

        case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
            dst->data.destructuring.targets = exprtk_node_copy(src->data.destructuring.targets, dest_arena);
            dst->data.destructuring.value = exprtk_node_copy(src->data.destructuring.value, dest_arena);
            dst->data.destructuring.is_constant = src->data.destructuring.is_constant;
            break;

        case EXPRTK_NODE_REST_PARAMETER:
            /* No extra data to copy besides type and positional info done in entry */
            break;

        case EXPRTK_NODE_SWITCH:
            dst->data.switch_stmt.value = exprtk_node_copy(src->data.switch_stmt.value, dest_arena);
            if (src->data.switch_stmt.case_count > 0) {
                dst->data.switch_stmt.cases = (exprtk_node_t**)mem_alloc(dest_arena, src->data.switch_stmt.case_count * 2 * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.switch_stmt.case_count * 2; ++i) {
                    dst->data.switch_stmt.cases[i] = exprtk_node_copy(src->data.switch_stmt.cases[i], dest_arena);
                }
            }
            if (src->data.switch_stmt.default_case) {
                dst->data.switch_stmt.default_case = exprtk_node_copy(src->data.switch_stmt.default_case, dest_arena);
            }
            break;

        case EXPRTK_NODE_DO_WHILE:
            dst->data.do_while.body = exprtk_node_copy(src->data.do_while.body, dest_arena);
            dst->data.do_while.condition = exprtk_node_copy(src->data.do_while.condition, dest_arena);
            break;

        case EXPRTK_NODE_MAP_LITERAL:
            if (src->data.map_literal.count > 0) {
                dst->data.map_literal.keys = (char**)mem_alloc(dest_arena, src->data.map_literal.count * sizeof(char*));
                dst->data.map_literal.values = (exprtk_node_t**)mem_alloc(dest_arena, src->data.map_literal.count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.map_literal.count; ++i) {
                    dst->data.map_literal.keys[i] = mem_strdup(dest_arena, src->data.map_literal.keys[i]);
                    dst->data.map_literal.values[i] = exprtk_node_copy(src->data.map_literal.values[i], dest_arena);
                }
            }
            break;

        case EXPRTK_NODE_MEMBER_ACCESS:
            dst->data.member_access.object = exprtk_node_copy(src->data.member_access.object, dest_arena);
            if (src->data.member_access.member)
                dst->data.member_access.member = mem_strdup(dest_arena, src->data.member_access.member);
            break;

        case EXPRTK_NODE_MEMBER_SET:
            dst->data.member_set.object = exprtk_node_copy(src->data.member_set.object, dest_arena);
            if (src->data.member_set.member)
                dst->data.member_set.member = mem_strdup(dest_arena, src->data.member_set.member);
            dst->data.member_set.value = exprtk_node_copy(src->data.member_set.value, dest_arena);
            break;

        case EXPRTK_NODE_FOR_IN:
            if (src->data.for_in.var_name)
                dst->data.for_in.var_name = mem_strdup(dest_arena, src->data.for_in.var_name);
            dst->data.for_in.collection = exprtk_node_copy(src->data.for_in.collection, dest_arena);
            dst->data.for_in.body = exprtk_node_copy(src->data.for_in.body, dest_arena);
            break;

        case EXPRTK_NODE_NULL:
            /* No data to copy */
            break;

        case EXPRTK_NODE_TEMPLATE_STRING:
            if (src->data.template_string.template_str) {
                dst->data.template_string.template_str = mem_strdup(dest_arena, src->data.template_string.template_str);
            }
            dst->data.template_string.len = src->data.template_string.len;
            break;

        case EXPRTK_NODE_TRY_CATCH:
            dst->data.try_catch.try_body = exprtk_node_copy(src->data.try_catch.try_body, dest_arena);
            if (src->data.try_catch.catch_var)
                dst->data.try_catch.catch_var = mem_strdup(dest_arena, src->data.try_catch.catch_var);
            dst->data.try_catch.catch_body = exprtk_node_copy(src->data.try_catch.catch_body, dest_arena);
            break;

        case EXPRTK_NODE_THROW:
            dst->data.throw_stmt.value = exprtk_node_copy(src->data.throw_stmt.value, dest_arena);
            break;
    }
    return dst;
}

// Create a new node using the arena
exprtk_node_t *exprtk_node_create(mem_pool_t *arena, exprtk_node_type_t type) {
    exprtk_node_t *n = MEM_ALLOC(arena, exprtk_node_t);
    if (n) {
        memset(n, 0, sizeof(exprtk_node_t));
        n->type = type;
        n->arena = arena;
        n->line = 0;
        n->column = 0;
    }
    return n;
}

exprtk_node_t *exprtk_fold_binary(exprtk_parse_ctx_t *ctx, int op, exprtk_node_t *left, exprtk_node_t *right) {
    if (!left || !right) return NULL;
    
    if (left->type == EXPRTK_NODE_NUMBER && right->type == EXPRTK_NODE_NUMBER) {
        double l = left->data.number;
        double r = right->data.number;
        double res = 0;
        int folded = 1;
        
        switch (op) {
            case exprtk_TOKEN_PLUS: res = l + r; break;
            case exprtk_TOKEN_MINUS: res = l - r; break;
            case exprtk_TOKEN_MULTIPLY: res = l * r; break;
            case exprtk_TOKEN_DIVIDE: if (r != 0) res = l / r; else folded = 0; break;
            case exprtk_TOKEN_MOD: if (r != 0) res = fmod(l, r); else folded = 0; break;
            case exprtk_TOKEN_POWER: res = pow(l, r); break;
            case exprtk_TOKEN_EQ: res = (l == r); break;
            case exprtk_TOKEN_NE: res = (l != r); break;
            case exprtk_TOKEN_LT: res = (l < r); break;
            case exprtk_TOKEN_LE: res = (l <= r); break;
            case exprtk_TOKEN_GT: res = (l > r); break;
            case exprtk_TOKEN_GE: res = (l >= r); break;
            case exprtk_TOKEN_AND: res = (l && r); break;
            case exprtk_TOKEN_OR: res = (l || r); break;
            default: folded = 0; break;
        }
        
        if (folded) {
            exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_NUMBER);
            if (n) {
                n->data.number = res;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }
    
    // String concatenation folding
    if (op == exprtk_TOKEN_PLUS && left->type == EXPRTK_NODE_STRING && right->type == EXPRTK_NODE_STRING) {
        size_t new_len = left->data.string.value.len + right->data.string.value.len;
        char *buf = (char*)mem_alloc(ctx->arena, new_len + 1);
        if (buf) {
            memcpy(buf, left->data.string.value.data, left->data.string.value.len);
            memcpy(buf + left->data.string.value.len, right->data.string.value.data, right->data.string.value.len);
            buf[new_len] = '\0';
            exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_STRING);
            if (n) {
                n->data.string.value.data = buf;
                n->data.string.value.len = new_len;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }

    // Number + String concatenation folding
    if (op == exprtk_TOKEN_PLUS && 
        ((left->type == EXPRTK_NODE_STRING && right->type == EXPRTK_NODE_NUMBER) ||
         (left->type == EXPRTK_NODE_NUMBER && right->type == EXPRTK_NODE_STRING))) {
        char n_buf[32];
        const char *l_data, *r_data;
        size_t l_len, r_len;

        if (left->type == EXPRTK_NODE_NUMBER) {
            l_len = snprintf(n_buf, sizeof(n_buf), "%g", left->data.number);
            l_data = n_buf;
        } else {
            l_data = left->data.string.value.data;
            l_len = left->data.string.value.len;
        }

        char r_buf[32];
        if (right->type == EXPRTK_NODE_NUMBER) {
            r_len = snprintf(r_buf, sizeof(r_buf), "%g", right->data.number);
            r_data = r_buf;
        } else {
            r_data = right->data.string.value.data;
            r_len = right->data.string.value.len;
        }

        size_t new_len = l_len + r_len;
        char *buf = (char*)mem_alloc(ctx->arena, new_len + 1);
        if (buf) {
            memcpy(buf, l_data, l_len);
            memcpy(buf + l_len, r_data, r_len);
            buf[new_len] = '\0';
            exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_STRING);
            if (n) {
                n->data.string.value.data = buf;
                n->data.string.value.len = new_len;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }

    exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = left;
    n->data.binary.right = right;
    return n;
}

exprtk_node_t *exprtk_fold_unary(exprtk_parse_ctx_t *ctx, int op, exprtk_node_t *child) {
    if (!child) return NULL;
    if (child->type == EXPRTK_NODE_NUMBER) {
        double v = child->data.number;
        if (op == exprtk_TOKEN_MINUS) {
            exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_NUMBER);
            n->data.number = -v;
            return n;
        }
        if (op == exprtk_TOKEN_PLUS) return child;
        if (op == exprtk_TOKEN_NOT) {
            exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_NUMBER);
            n->data.number = (fabs(v) < 1e-9 ? 1.0 : 0.0);
            return n;
        }
    }
    
    exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = NULL;
    n->data.binary.right = child;
    return n;
}

exprtk_node_t *exprtk_fold_if(exprtk_parse_ctx_t *ctx, exprtk_node_t *cond, exprtk_node_t *if_branch, exprtk_node_t *else_branch) {
    if (cond->type == EXPRTK_NODE_NUMBER) {
        double v = cond->data.number;
        if (fabs(v) > 1e-9) {
            return if_branch;
        } else {
            return else_branch ? else_branch : exprtk_node_create(ctx->arena, EXPRTK_NODE_BLOCK);
        }
    }
    exprtk_node_t *n = exprtk_node_create(ctx->arena, EXPRTK_NODE_IF);
    n->data.if_stmt.condition = cond;
    n->data.if_stmt.if_branch = if_branch;
    n->data.if_stmt.else_branch = else_branch;
    return n;
}

exprtk_node_t *exprtk_parse_ext(const char *input, size_t length,
                                 mem_pool_t *arena, int *error,
                                 char *error_msg, size_t error_msg_len) {
    if (!input) return NULL;
    if (length == 0) length = strlen(input);

    exprtk_lexer_t lexer;
    exprtk_lexer_init(&lexer, input, length);

    exprtk_parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena = arena;

    void *parser = exprtkParseAlloc(malloc);
    if (!parser) {
        if (error) *error = 1;
        return NULL;
    }

    exprtk_token_t token;
    memset(&token, 0, sizeof(token));
    int ret;
    while ((ret = exprtk_lexer_next(&lexer, &token)) > 0) {
        exprtkParse(parser, ret, token, &ctx);
        if (ctx.fatal_error) break;
    }

    if (ret < 0) {
        ctx.error = 1;
        ctx.fatal_error = 1;
        if (ctx.error_msg[0] == '\0') {
            if (lexer.error[0] != '\0') {
                snprintf(ctx.error_msg, sizeof(ctx.error_msg), "%s", lexer.error);
            } else {
                snprintf(ctx.error_msg, sizeof(ctx.error_msg), "Lexer error at line %d", lexer.line);
            }
        }
    } else if (!ctx.fatal_error) {
        exprtk_token_t end_token;
        memset(&end_token, 0, sizeof(end_token));
        exprtkParse(parser, 0, end_token, &ctx);
    }

    exprtkParseFree(parser, free);

    if (error) *error = ctx.error;
    if (ctx.error && error_msg && error_msg_len > 0) {
        strncpy(error_msg, ctx.error_msg, error_msg_len - 1);
        error_msg[error_msg_len - 1] = '\0';
    }

    return ctx.error ? NULL : ctx.root;
}

exprtk_node_t *exprtk_parse(const char *input, size_t length) {
    mem_pool_t *arena = (mem_pool_t*)malloc(sizeof(mem_pool_t));
    if (!arena || mem_init(arena, 4096) != 0) {
        if (arena) free(arena);
        return NULL;
    }
    int err = 0;
    exprtk_node_t *root = exprtk_parse_ext(input, length, arena, &err, NULL, 0);
    if (err || !root) {
        mem_destroy(arena);
        free(arena);
        return NULL;
    }
    return root;
}

void exprtk_free(exprtk_node_t *node) {
    if (!node || !node->arena) return;
    mem_pool_t *arena = node->arena;
    mem_destroy(arena);
    free(arena);
}

size_t exprtk_node_count(const exprtk_node_t *node) {
    if (!node) return 0;
    size_t count = 1;
    switch (node->type) {
        case EXPRTK_NODE_BINARY_OP:
            if (node->data.binary.left) count += exprtk_node_count(node->data.binary.left);
            count += exprtk_node_count(node->data.binary.right);
            break;
        case EXPRTK_NODE_IF:
            count += exprtk_node_count(node->data.if_stmt.condition);
            count += exprtk_node_count(node->data.if_stmt.if_branch);
            if (node->data.if_stmt.else_branch) count += exprtk_node_count(node->data.if_stmt.else_branch);
            break;
        case EXPRTK_NODE_WHILE:
            count += exprtk_node_count(node->data.while_loop.condition);
            count += exprtk_node_count(node->data.while_loop.body);
            break;
        case EXPRTK_NODE_FOR:
            if (node->data.for_loop.init) count += exprtk_node_count(node->data.for_loop.init);
            if (node->data.for_loop.condition) count += exprtk_node_count(node->data.for_loop.condition);
            if (node->data.for_loop.post) count += exprtk_node_count(node->data.for_loop.post);
            count += exprtk_node_count(node->data.for_loop.body);
            break;
        case EXPRTK_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                count += exprtk_node_count(node->data.block.statements[i]);
            }
            break;
        case EXPRTK_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                count += exprtk_node_count(node->data.function.args[i]);
            }
            break;
        case EXPRTK_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                count += exprtk_node_count(node->data.vector.elements[i]);
            }
            break;
        case EXPRTK_NODE_INDEX:
            count += exprtk_node_count(node->data.index_access.array);
            count += exprtk_node_count(node->data.index_access.index);
            break;
        case EXPRTK_NODE_SLICE:
            count += exprtk_node_count(node->data.slice.array);
            if (node->data.slice.start) count += exprtk_node_count(node->data.slice.start);
            if (node->data.slice.end) count += exprtk_node_count(node->data.slice.end);
            break;
        case EXPRTK_NODE_FUNCTION_EXPRESSION:
        case EXPRTK_NODE_FUNCTION_DEFINITION:
            for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
                count += exprtk_node_count(node->data.func_def.arg_params[i]);
            }
            count += exprtk_node_count(node->data.func_def.body);
            break;
        case EXPRTK_NODE_MEMBER_CALL:
            count += exprtk_node_count(node->data.member_call.object);
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i)
                count += exprtk_node_count(node->data.member_call.args[i]);
            break;
        case EXPRTK_NODE_ASSIGNMENT:
            count += exprtk_node_count(node->data.assignment.value);
            break;
        case EXPRTK_NODE_FLOW:
            if (node->data.flow.value) count += exprtk_node_count(node->data.flow.value);
            break;
        case EXPRTK_NODE_CONSTANT_DECL:
            count += exprtk_node_count(node->data.assignment.value);
            break;
        case EXPRTK_NODE_SWITCH:
            count += exprtk_node_count(node->data.switch_stmt.value);
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                count += exprtk_node_count(node->data.switch_stmt.cases[i]);
            }
            if (node->data.switch_stmt.default_case) count += exprtk_node_count(node->data.switch_stmt.default_case);
            break;
        case EXPRTK_NODE_DO_WHILE:
            count += exprtk_node_count(node->data.do_while.body);
            count += exprtk_node_count(node->data.do_while.condition);
            break;
        case EXPRTK_NODE_MAP_LITERAL:
            for (size_t i = 0; i < node->data.map_literal.count; ++i)
                count += exprtk_node_count(node->data.map_literal.values[i]);
            break;
        case EXPRTK_NODE_MEMBER_ACCESS:
            count += exprtk_node_count(node->data.member_access.object);
            break;
        case EXPRTK_NODE_MEMBER_SET:
            count += exprtk_node_count(node->data.member_set.object);
            count += exprtk_node_count(node->data.member_set.value);
            break;
        case EXPRTK_NODE_FOR_IN:
            count += exprtk_node_count(node->data.for_in.collection);
            count += exprtk_node_count(node->data.for_in.body);
            break;
        case EXPRTK_NODE_SPREAD:
            count += exprtk_node_count(node->data.spread.child);
            break;

        case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
            count += exprtk_node_count(node->data.destructuring.targets);
            count += exprtk_node_count(node->data.destructuring.value);
            break;
        case EXPRTK_NODE_REST_PARAMETER:
            break;

        case EXPRTK_NODE_TRY_CATCH:
            count += exprtk_node_count(node->data.try_catch.try_body);
            count += exprtk_node_count(node->data.try_catch.catch_body);
            break;
        case EXPRTK_NODE_THROW:
            if (node->data.throw_stmt.value) count += exprtk_node_count(node->data.throw_stmt.value);
            break;

        case EXPRTK_NODE_NULL:
            break;
        default: break;
    }
    return count;
}

size_t exprtk_node_depth(const exprtk_node_t *node) {
    if (!node) return 0;
    size_t max_d = 0;
    size_t d;
    switch (node->type) {
        case EXPRTK_NODE_BINARY_OP:
            if (node->data.binary.left) { d = exprtk_node_depth(node->data.binary.left); if (d > max_d) max_d = d; }
            d = exprtk_node_depth(node->data.binary.right); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_IF:
            d = exprtk_node_depth(node->data.if_stmt.condition); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.if_stmt.if_branch); if (d > max_d) max_d = d;
            if (node->data.if_stmt.else_branch) {
                d = exprtk_node_depth(node->data.if_stmt.else_branch); if (d > max_d) max_d = d;
            }
            break;
        case EXPRTK_NODE_WHILE:
            d = exprtk_node_depth(node->data.while_loop.condition); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.while_loop.body); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_FOR:
            if (node->data.for_loop.init) { d = exprtk_node_depth(node->data.for_loop.init); if (d > max_d) max_d = d; }
            if (node->data.for_loop.condition) { d = exprtk_node_depth(node->data.for_loop.condition); if (d > max_d) max_d = d; }
            if (node->data.for_loop.post) { d = exprtk_node_depth(node->data.for_loop.post); if (d > max_d) max_d = d; }
            d = exprtk_node_depth(node->data.for_loop.body); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                d = exprtk_node_depth(node->data.block.statements[i]); if (d > max_d) max_d = d;
            }
            break;
        case EXPRTK_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                d = exprtk_node_depth(node->data.function.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case EXPRTK_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                d = exprtk_node_depth(node->data.vector.elements[i]); if (d > max_d) max_d = d;
            }
            break;
        case EXPRTK_NODE_INDEX:
            d = exprtk_node_depth(node->data.index_access.array); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.index_access.index); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_SLICE:
            d = exprtk_node_depth(node->data.slice.array); if (d > max_d) max_d = d;
            if (node->data.slice.start) { d = exprtk_node_depth(node->data.slice.start); if (d > max_d) max_d = d; }
            if (node->data.slice.end) { d = exprtk_node_depth(node->data.slice.end); if (d > max_d) max_d = d; }
            break;
        case EXPRTK_NODE_FUNCTION_EXPRESSION:
        case EXPRTK_NODE_FUNCTION_DEFINITION:
            for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
                d = exprtk_node_depth(node->data.func_def.arg_params[i]); if (d > max_d) max_d = d;
            }
            d = exprtk_node_depth(node->data.func_def.body); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_MEMBER_CALL:
            d = exprtk_node_depth(node->data.member_call.object); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
                d = exprtk_node_depth(node->data.member_call.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case EXPRTK_NODE_INSTANCEOF:
            d = exprtk_node_depth(node->data.instanceof_expr.object); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.instanceof_expr.class_expr); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_ASSIGNMENT:
            d = exprtk_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_FLOW:
            if (node->data.flow.value) { d = exprtk_node_depth(node->data.flow.value); if (d > max_d) max_d = d; }
            break;
        case EXPRTK_NODE_CONSTANT_DECL:
            d = exprtk_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_SWITCH:
            d = exprtk_node_depth(node->data.switch_stmt.value); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                d = exprtk_node_depth(node->data.switch_stmt.cases[i]); if (d > max_d) max_d = d;
            }
            if (node->data.switch_stmt.default_case) { d = exprtk_node_depth(node->data.switch_stmt.default_case); if (d > max_d) max_d = d; }
            break;
        case EXPRTK_NODE_DO_WHILE:
            d = exprtk_node_depth(node->data.do_while.body); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.do_while.condition); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_MAP_LITERAL:
            for (size_t i = 0; i < node->data.map_literal.count; ++i) {
                d = exprtk_node_depth(node->data.map_literal.values[i]); if (d > max_d) max_d = d;
            }
            break;
        case EXPRTK_NODE_MEMBER_ACCESS:
            d = exprtk_node_depth(node->data.member_access.object); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_MEMBER_SET:
            d = exprtk_node_depth(node->data.member_set.object); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.member_set.value); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_FOR_IN:
            d = exprtk_node_depth(node->data.for_in.collection); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.for_in.body); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_SPREAD:
            d = exprtk_node_depth(node->data.spread.child); if (d > max_d) max_d = d;
            break;

        case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
            d = exprtk_node_depth(node->data.destructuring.targets); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.destructuring.value); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_REST_PARAMETER:
            break;

        case EXPRTK_NODE_TRY_CATCH:
            d = exprtk_node_depth(node->data.try_catch.try_body); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.try_catch.catch_body); if (d > max_d) max_d = d;
            break;
        case EXPRTK_NODE_THROW:
            if (node->data.throw_stmt.value) { d = exprtk_node_depth(node->data.throw_stmt.value); if (d > max_d) max_d = d; }
            break;

        case EXPRTK_NODE_NULL:
            break;
        default: break;
    }
    return 1 + max_d;
}
