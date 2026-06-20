/**
 * @file exprtk_eval.c
 * @brief core evaluation engine for exprtk AST
 */

#include "exprtk.h"
#include "exprtk_internal.h"
#include "exprtk_lexer.h"
#include "exprtk_grammar_gen.h"
#include "exprtk_module.h"
#include "exprtk_class.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!node || (env && env->aborted)) return zero;

    if (env) {
        // Track last evaluated node position for error reporting
        if (node->line > 0) {
            env->last_line = node->line;
            env->last_column = node->column;
        }

        // Node Count Limit
        env->curr_nodes++;
        if (env->curr_nodes > env->max_nodes) {
            env->aborted = 1;
            return zero;
        }
    }

    switch (node->type) {
        case EXPRTK_NODE_NUMBER:
            return exprtk_val_num(node->data.number);
        case EXPRTK_NODE_INTEGER:
            return exprtk_val_int(node->data.integer);
        case EXPRTK_NODE_STRING:
            return exprtk_val_str(node->data.string.value);
        case EXPRTK_NODE_TEMPLATE_STRING: {
            const char *str = node->data.template_string.template_str;
            size_t len = node->data.template_string.len;
            char *result_buf = NULL;
            size_t result_len = 0;
            size_t result_cap = 0;

#define APPEND_STR(s, slen) do { \
    if (result_len + (slen) + 1 > result_cap) { \
        result_cap = (result_cap == 0) ? 64 : result_cap * 2; \
        if (result_cap < result_len + (slen) + 1) result_cap = result_len + (slen) + 1; \
        char *new_buf = (char*)mem_alloc(node->arena, result_cap); \
        if (result_buf) memcpy(new_buf, result_buf, result_len); \
        result_buf = new_buf; \
    } \
    memcpy(result_buf + result_len, (s), (slen)); \
    result_len += (slen); \
    result_buf[result_len] = '\0'; \
} while(0)

            size_t i = 0;
            while (i < len) {
                if (str[i] == '$' && i + 1 < len && str[i+1] == '{') {
                    // Start of expression
                    i += 2;
                    size_t expr_start = i;
                    int depth = 1;
                    while (i < len && depth > 0) {
                        if (str[i] == '{') depth++;
                        else if (str[i] == '}') depth--;
                        i++;
                    }
                    if (depth == 0) {
                        size_t expr_len = i - 1 - expr_start;
                        if (expr_len > 0) {
                            char *expr_str = (char*)malloc(expr_len + 1);
                            memcpy(expr_str, str + expr_start, expr_len);
                            expr_str[expr_len] = '\0';

                            // Parse and evaluate the inner expression
                            exprtk_node_t *expr_node = exprtk_parse(expr_str, expr_len);
                            if (expr_node) {
                                exprtk_value_t expr_val = exprtk_eval(expr_node, env);
                                if (expr_val.type == EXPRTK_VAL_INTEGER) {
                                    char num_buf[64];
                                    snprintf(num_buf, sizeof(num_buf), "%lld", (long long)expr_val.data.integer);
                                    APPEND_STR(num_buf, strlen(num_buf));
                                } else if (expr_val.type == EXPRTK_VAL_NUMBER) {
                                    char num_buf[64];
                                    snprintf(num_buf, sizeof(num_buf), "%g", expr_val.data.number);
                                    APPEND_STR(num_buf, strlen(num_buf));
                                } else if (expr_val.type == EXPRTK_VAL_STRING) {
                                    APPEND_STR(expr_val.data.string.data, expr_val.data.string.len);
                                } else if (expr_val.type == EXPRTK_VAL_NULL) {
                                    APPEND_STR("null", 4);
                                } else if (expr_val.type == EXPRTK_VAL_VECTOR) {
                                    APPEND_STR("[vector]", 8);
                                } else if (expr_val.type == EXPRTK_VAL_MAP) {
                                    APPEND_STR("[map]", 5);
                                } else if (expr_val.type == EXPRTK_VAL_LIST) {
                                    APPEND_STR("[list]", 6);
                                }
                                exprtk_free(expr_node);
                            }
                            free(expr_str);
                        }
                    }
                } else {
                    // Normal character
                    APPEND_STR(&str[i], 1);
                    i++;
                }
            }
            if (!result_buf) {
                result_buf = (char*)mem_alloc(node->arena, 1);
                result_buf[0] = '\0';
            }
            tstr_v sv;
            sv.data = result_buf;
            sv.len = result_len;
#undef APPEND_STR
            return exprtk_val_str(sv);
        }
        case EXPRTK_NODE_VARIABLE: {
            const char *name = node->data.variable.name;
            return exprtk_env_get(env, name);
        }
        case EXPRTK_NODE_SPREAD:
            return exprtk_eval(node->data.spread.child, env);
        case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT: {
            exprtk_value_t rhs = exprtk_eval(node->data.destructuring.value, env);
            eval_destructure(node->data.destructuring.targets, rhs, env, node->data.destructuring.is_constant);
            return rhs;
        }
        case EXPRTK_NODE_ASSIGNMENT: {
            exprtk_value_t val = exprtk_eval(node->data.assignment.value, env);
            exprtk_env_set(env, node->data.assignment.name, val);
            return val;
        }
        case EXPRTK_NODE_CONSTANT_DECL: {
            exprtk_value_t val = exprtk_eval(node->data.assignment.value, env);
            exprtk_env_set_constant(env, node->data.assignment.name, val);
            return val;
        }
        case EXPRTK_NODE_IF: {
            exprtk_value_t cond_val = exprtk_eval(node->data.if_stmt.condition, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;

            double cond = 0.0;
            if (cond_val.type == EXPRTK_VAL_INTEGER) {
                cond = (double)cond_val.data.integer;
            } else if (cond_val.type == EXPRTK_VAL_NUMBER) {
                cond = cond_val.data.number;
            } else if (cond_val.type == EXPRTK_VAL_STRING) {
                cond = (double)(cond_val.data.string.len > 0);
            }

            if (fabs(cond) > 1e-9) {
                if (node->data.if_stmt.if_branch) return exprtk_eval(node->data.if_stmt.if_branch, env);
            } else {
                if (node->data.if_stmt.else_branch) return exprtk_eval(node->data.if_stmt.else_branch, env);
            }
            return zero;
        }
        case EXPRTK_NODE_WHILE: {
            exprtk_value_t last_val = zero;
            while (1) {
                if (env && env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE) break;
                if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;

                exprtk_value_t cond_val = exprtk_eval(node->data.while_loop.condition, env);
                if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;

                double cond = 0.0;
                if (cond_val.type == EXPRTK_VAL_INTEGER) {
                    cond = (double)cond_val.data.integer;
                } else if (cond_val.type == EXPRTK_VAL_NUMBER) {
                    cond = cond_val.data.number;
                } else if (cond_val.type == EXPRTK_VAL_STRING) {
                    cond = (double)(cond_val.data.string.len > 0);
                }
                if (fabs(cond) <= 1e-9) break;

                if (env) {
                    env->curr_loop_iterations++;
                    if (env->curr_loop_iterations > env->max_loop_iterations) {
                        env->aborted = 1;
                        break;
                    }
                }

                last_val = exprtk_eval(node->data.while_loop.body, env);
                if (env && env->flow == exprtk_FLOW_BREAK) {
                    env->flow = exprtk_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == exprtk_FLOW_RETURN) break;
            }
            return last_val;
        }
        case EXPRTK_NODE_FOR: {
            if (node->data.for_loop.init) exprtk_eval(node->data.for_loop.init, env);
            if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) return zero;

            exprtk_value_t last_val = zero;
            while (1) {
                if (node->data.for_loop.condition) {
                    exprtk_value_t cond_val = exprtk_eval(node->data.for_loop.condition, env);
                    if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
                    double cond = 0.0;
                    if (cond_val.type == EXPRTK_VAL_INTEGER) {
                        cond = (double)cond_val.data.integer;
                    } else if (cond_val.type == EXPRTK_VAL_NUMBER) {
                        cond = cond_val.data.number;
                    } else if (cond_val.type == EXPRTK_VAL_STRING) {
                        cond = (double)(cond_val.data.string.len > 0);
                    }
                    if (fabs(cond) <= 1e-9) break;
                }

                if (env) {
                    env->curr_loop_iterations++;
                    if (env->curr_loop_iterations > env->max_loop_iterations) {
                        env->aborted = 1;
                        break;
                    }
                }

                last_val = exprtk_eval(node->data.for_loop.body, env);
                if (env && env->flow == exprtk_FLOW_BREAK) {
                    env->flow = exprtk_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == exprtk_FLOW_RETURN) break;

                if (node->data.for_loop.post) {
                    exprtk_eval(node->data.for_loop.post, env);
                    if (env && (env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE)) break;
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                }
            }
            return last_val;
        }
        case EXPRTK_NODE_BLOCK: {
            exprtk_value_t last_val = zero;
            for (size_t i = 0; i < node->data.block.count; ++i) {
                last_val = exprtk_eval(node->data.block.statements[i], env);
                if (env && env->flow != exprtk_FLOW_NORMAL) break;
                if (env && env->aborted) break;
            }
            return last_val;
        }
        case EXPRTK_NODE_FLOW: {
            if (env) {
                if (node->data.flow.type == exprtk_TOKEN_RETURN) {
                    if (node->data.flow.value) {
                        env->return_value = exprtk_eval(node->data.flow.value, env);
                        if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
                            return env->return_value;
                        }
                    } else {
                        env->return_value = zero;
                    }
                    env->flow = exprtk_FLOW_RETURN;
                    return env->return_value;
                } else if (node->data.flow.type == exprtk_TOKEN_BREAK) {
                    env->flow = exprtk_FLOW_BREAK;
                } else if (node->data.flow.type == exprtk_TOKEN_CONTINUE) {
                    env->flow = exprtk_FLOW_CONTINUE;
                }
            }
            return zero;
        }
        case EXPRTK_NODE_BINARY_OP: {
            exprtk_value_t l_val = {EXPRTK_VAL_NUMBER, {0.0}};
            if (node->data.binary.left) {
                l_val = exprtk_eval(node->data.binary.left, env);
                if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
            }

            exprtk_value_t r_val = exprtk_eval(node->data.binary.right, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;

            // INTEGER + INTEGER → INTEGER
            if (l_val.type == EXPRTK_VAL_INTEGER && r_val.type == EXPRTK_VAL_INTEGER) {
                int64_t l = l_val.data.integer;
                int64_t r = r_val.data.integer;
                switch (node->data.binary.op) {
                    case exprtk_TOKEN_PLUS:     return exprtk_val_int(l + r);
                    case exprtk_TOKEN_MINUS:    return exprtk_val_int(l - r);
                    case exprtk_TOKEN_MULTIPLY: return exprtk_val_int(l * r);
                    case exprtk_TOKEN_DIVIDE:
                        if (r == 0) return throw_error(env, node, "Division by zero");
                        if (l % r == 0) return exprtk_val_int(l / r);
                        return exprtk_val_num((double)l / (double)r);
                    case exprtk_TOKEN_MOD:
                        if (r == 0) return throw_error(env, node, "Modulo by zero");
                        return exprtk_val_int(l % r);
                    case exprtk_TOKEN_POWER:    return exprtk_val_num(pow((double)l, (double)r));
                    case exprtk_TOKEN_EQ:       return exprtk_val_int(l == r);
                    case exprtk_TOKEN_NE:       return exprtk_val_int(l != r);
                    case exprtk_TOKEN_LT:       return exprtk_val_int(l < r);
                    case exprtk_TOKEN_LE:       return exprtk_val_int(l <= r);
                    case exprtk_TOKEN_GT:       return exprtk_val_int(l > r);
                    case exprtk_TOKEN_GE:       return exprtk_val_int(l >= r);
                    case exprtk_TOKEN_AND:      return exprtk_val_int(l && r);
                    case exprtk_TOKEN_OR:       return exprtk_val_int(l || r);
                }
            }

            // (INTEGER | NUMBER) + (INTEGER | NUMBER) → NUMBER (type promotion)
            if ((l_val.type == EXPRTK_VAL_INTEGER || l_val.type == EXPRTK_VAL_NUMBER) &&
                (r_val.type == EXPRTK_VAL_INTEGER || r_val.type == EXPRTK_VAL_NUMBER)) {
                double l = (l_val.type == EXPRTK_VAL_INTEGER) ? (double)l_val.data.integer : l_val.data.number;
                double r = (r_val.type == EXPRTK_VAL_INTEGER) ? (double)r_val.data.integer : r_val.data.number;
                switch (node->data.binary.op) {
                    case exprtk_TOKEN_PLUS:     return exprtk_val_num(l + r);
                    case exprtk_TOKEN_MINUS:    return exprtk_val_num(l - r);
                    case exprtk_TOKEN_MULTIPLY: return exprtk_val_num(l * r);
                    case exprtk_TOKEN_DIVIDE:
                        if (fabs(r) < 1e-15) return throw_error(env, node, "Division by zero");
                        return exprtk_val_num(l / r);
                    case exprtk_TOKEN_MOD:
                        if (fabs(r) < 1e-15) return throw_error(env, node, "Modulo by zero");
                        return exprtk_val_num(fmod(l, r));
                    case exprtk_TOKEN_POWER:    return exprtk_val_num(pow(l, r));
                    case exprtk_TOKEN_EQ:       return exprtk_val_num(fabs(l - r) < 1e-9);
                    case exprtk_TOKEN_NE:       return exprtk_val_num(fabs(l - r) >= 1e-9);
                    case exprtk_TOKEN_LT:       return exprtk_val_num(l < r);
                    case exprtk_TOKEN_LE:       return exprtk_val_num(l <= r);
                    case exprtk_TOKEN_GT:       return exprtk_val_num(l > r);
                    case exprtk_TOKEN_GE:       return exprtk_val_num(l >= r);
                    case exprtk_TOKEN_AND:      return exprtk_val_num(fabs(l) > 1e-9 && fabs(r) > 1e-9);
                    case exprtk_TOKEN_OR:       return exprtk_val_num(fabs(l) > 1e-9 || fabs(r) > 1e-9);
                }
            }

            // STRING + ANY → STRING (concatenation)
            if (node->data.binary.op == exprtk_TOKEN_PLUS &&
                (l_val.type == EXPRTK_VAL_STRING || r_val.type == EXPRTK_VAL_STRING)) {
                char n_buf[32], r_buf[32];
                const char *l_data, *r_data;
                size_t l_len, r_len;

                if (l_val.type == EXPRTK_VAL_STRING) {
                    l_data = l_val.data.string.data;
                    l_len = l_val.data.string.len;
                } else if (l_val.type == EXPRTK_VAL_INTEGER) {
                    l_len = snprintf(n_buf, sizeof(n_buf), "%lld", (long long)l_val.data.integer);
                    l_data = n_buf;
                } else {
                    l_len = snprintf(n_buf, sizeof(n_buf), "%g", l_val.data.number);
                    l_data = n_buf;
                }

                if (r_val.type == EXPRTK_VAL_STRING) {
                    r_data = r_val.data.string.data;
                    r_len = r_val.data.string.len;
                } else if (r_val.type == EXPRTK_VAL_INTEGER) {
                    r_len = snprintf(r_buf, sizeof(r_buf), "%lld", (long long)r_val.data.integer);
                    r_data = r_buf;
                } else {
                    r_len = snprintf(r_buf, sizeof(r_buf), "%g", r_val.data.number);
                    r_data = r_buf;
                }

                size_t new_len = l_len + r_len;
                char *new_data = (char*)mem_alloc(node->arena, new_len + 1);
                memcpy(new_data, l_data, l_len);
                memcpy(new_data + l_len, r_data, r_len);
                new_data[new_len] = '\0';
                return exprtk_val_str(tstr_v_from_buf(new_data, new_len));
            }

            // STRING == STRING
            if (l_val.type == EXPRTK_VAL_STRING && r_val.type == EXPRTK_VAL_STRING) {
                if (node->data.binary.op == exprtk_TOKEN_EQ) {
                    return exprtk_val_num(tstr_v_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                }
                if (node->data.binary.op == exprtk_TOKEN_NE) {
                    return exprtk_val_num(!tstr_v_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                }
            }

            // NULL == NULL
            if (l_val.type == EXPRTK_VAL_NULL && r_val.type == EXPRTK_VAL_NULL) {
                if (node->data.binary.op == exprtk_TOKEN_EQ) return exprtk_val_num(1.0);
                if (node->data.binary.op == exprtk_TOKEN_NE) return exprtk_val_num(0.0);
            }

            // NULL == ANY (not NULL) → false
            if ((l_val.type == EXPRTK_VAL_NULL || r_val.type == EXPRTK_VAL_NULL) &&
                (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
                return exprtk_val_num(node->data.binary.op == exprtk_TOKEN_NE ? 1.0 : 0.0);
            }

            // Type error: unsupported operation
            const char *op_name = "unknown";
            switch (node->data.binary.op) {
                case exprtk_TOKEN_PLUS: op_name = "+"; break;
                case exprtk_TOKEN_MINUS: op_name = "-"; break;
                case exprtk_TOKEN_MULTIPLY: op_name = "*"; break;
                case exprtk_TOKEN_DIVIDE: op_name = "/"; break;
                case exprtk_TOKEN_MOD: op_name = "%"; break;
                case exprtk_TOKEN_POWER: op_name = "**"; break;
                case exprtk_TOKEN_LT: op_name = "<"; break;
                case exprtk_TOKEN_LE: op_name = "<="; break;
                case exprtk_TOKEN_GT: op_name = ">"; break;
                case exprtk_TOKEN_GE: op_name = ">="; break;
                case exprtk_TOKEN_AND: op_name = "&&"; break;
                case exprtk_TOKEN_OR: op_name = "||"; break;
                default: break;
            }
            return throw_error(env, node, "Type error: cannot apply '%s' to %s and %s",
                             op_name, type_name(l_val.type), type_name(r_val.type));
        }
        case EXPRTK_NODE_FUNCTION_CALL: {
            if (!node->data.function.name) return zero;

            exprtk_value_t callee = exprtk_env_get(env, node->data.function.name);
            if (callee.type == EXPRTK_VAL_CLASS) {
                return eval_class_value_instantiation(callee.data.class_val.klass,
                                                      node->data.function.name,
                                                      node->data.function.args,
                                                      node->data.function.arg_count,
                                                      env);
            }

            // Regular function call
            size_t actual_count = 0;
            exprtk_value_t *args = eval_expand_args(node->data.function.args, node->data.function.arg_count, env, &actual_count);
            if (!args && actual_count == 0) return zero;
            if (callee.type == EXPRTK_VAL_BOUND_METHOD) {
                exprtk_value_t result = eval_instance_script_method(
                    callee.data.bound_method_val.instance,
                    callee.data.bound_method_val.method,
                    actual_count, args, env);
                free(args);
                return result;
            }
            if (callee.type == EXPRTK_VAL_FUNCTION && callee.data.function.body) {
                exprtk_value_t result =
                    exprtk_call_function_value(callee, actual_count, args, env);
                free(args);
                return result;
            }

            exprtk_value_t result = exprtk_call_internal(node->data.function.name, actual_count, args, env, &env->arena);
            free(args);
            return result;
        }
        case EXPRTK_NODE_NEW:
            return eval_class_instantiation(node->data.new_expr.class_name,
                                            node->data.new_expr.args,
                                            node->data.new_expr.arg_count,
                                            env);
        case EXPRTK_NODE_FUNCTION_DEFINITION: {
            const char *name = node->data.func_def.name;
            if (!name) return zero;

            exprtk_func_t *curr = env->funcs;
            while (curr) {
                if (curr->name && strcmp(curr->name, name) == 0) break;
                curr = curr->next;
            }
            if (!curr) {
                curr = (exprtk_func_t*)calloc(1, sizeof(exprtk_func_t));
                curr->name = strdup(name);
                curr->next = env->funcs;
                env->funcs = curr;
            }
            curr->is_script = 1;
            curr->access_level = EXPRTK_ACCESS_PUBLIC;
            curr->data.script.arg_count = node->data.func_def.arg_count;
            if (curr->data.script.arg_count > 0) {
                curr->data.script.arg_params = (exprtk_node_t**)calloc(curr->data.script.arg_count, sizeof(exprtk_node_t*));
                for (size_t i = 0; i < curr->data.script.arg_count; ++i) {
                    curr->data.script.arg_params[i] = exprtk_node_copy(node->data.func_def.arg_params[i], &env->arena);
                }
            } else {
                curr->data.script.arg_params = NULL;
            }
            curr->data.script.body = exprtk_node_copy(node->data.func_def.body, &env->arena);
            return zero;
        }
        case EXPRTK_NODE_MEMBER_CALL: {
            size_t mc_argc = 0;
            exprtk_value_t *mc_args = eval_expand_args(
                node->data.member_call.args, node->data.member_call.arg_count, env, &mc_argc);
            if (!mc_args && mc_argc == 0) return zero;

            mc_ctx_t mc = {
                .method   = node->data.member_call.method,
                .obj      = exprtk_eval(node->data.member_call.object, env),
                .args     = mc_args,
                .argc     = mc_argc,
                .obj_node = node->data.member_call.object,
                .env      = env,
                .arena    = node->arena,
            };

            exprtk_value_t mc_result;
            switch (mc.obj.type) {
                case EXPRTK_VAL_LIST:   mc_result = eval_list_method(&mc);   break;
                case EXPRTK_VAL_MAP:    mc_result = eval_map_method(&mc);    break;
                case EXPRTK_VAL_STRING: mc_result = eval_string_method(&mc); break;
                case EXPRTK_VAL_VECTOR: mc_result = eval_vector_method(&mc); break;
                // OOP: Instance method call
                case EXPRTK_VAL_INSTANCE: {
                    exprtk_instance_t *instance = mc.obj.data.instance_val.instance;
                    exprtk_func_t *method =
                        exprtk_class_lookup_method_typed(instance->klass, mc.method, 0,
                                                         mc_argc, mc_args);
                    if (!method) {
                        mc_result = throw_error(env, node, "Instance has no method '%s'", mc.method);
                    } else if (!can_access_method(env, method, mc.obj_node)) {
                        mc_result = throw_method_access_error(env, node, mc.method, method);
                    } else {
                        mc_result = eval_instance_script_method(instance, method,
                                                                mc_argc, mc_args, env);
                    }
                    break;
                }
                // OOP: Bound method call (when method was stored in a variable)
                case EXPRTK_VAL_BOUND_METHOD: {
                    exprtk_instance_t *instance = mc.obj.data.bound_method_val.instance;
                    exprtk_func_t *method = mc.obj.data.bound_method_val.method;

                    mc_result = eval_instance_script_method(instance, method,
                                                            mc_argc, mc_args, env);
                    break;
                }
                // OOP: Static class method call
                case EXPRTK_VAL_CLASS: {
                    exprtk_class_t *klass = mc.obj.data.class_val.klass;
                    exprtk_func_t *method =
                        exprtk_class_lookup_method_typed(klass, mc.method, 1, mc_argc, mc_args);
                    if (!method) {
                        mc_result = throw_error(env, node, "Class has no static method '%s'", mc.method);
                    } else if (!can_access_method(env, method, mc.obj_node)) {
                        mc_result = throw_method_access_error(env, node, mc.method, method);
                    } else {
                        mc_result = eval_script_function(method, mc_argc, mc_args, env, env);
                    }
                    break;
                }
                default:
                    if (mc.obj_node->type == EXPRTK_NODE_VARIABLE) {
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s.%s",
                                 mc.obj_node->data.variable.name, mc.method);
                        mc_result = exprtk_call_internal(full_name, mc_argc, mc_args, env, &env->arena);
                    } else {
                        mc_result = throw_error(env, node, "Method call '%s' is invalid for %s",
                                                mc.method ? mc.method : "<null>",
                                                type_name(mc.obj.type));
                    }
                    break;
            }
            free(mc_args);
            return mc_result;
        }
        case EXPRTK_NODE_DO_WHILE: {
            exprtk_value_t last_val = zero;
            while (1) {
                last_val = exprtk_eval(node->data.do_while.body, env);
                if (env && env->flow == exprtk_FLOW_BREAK) {
                    env->flow = exprtk_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == exprtk_FLOW_RETURN) break;
                if (env && env->flow == exprtk_FLOW_CONTINUE) {
                    env->flow = exprtk_FLOW_NORMAL;
                }

                exprtk_value_t cond_val = exprtk_eval(node->data.do_while.condition, env);
                if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;

                double cond = 0.0;
                if (cond_val.type == EXPRTK_VAL_INTEGER) {
                    cond = (double)cond_val.data.integer;
                } else if (cond_val.type == EXPRTK_VAL_NUMBER) {
                    cond = cond_val.data.number;
                } else if (cond_val.type == EXPRTK_VAL_STRING) {
                    cond = (double)(cond_val.data.string.len > 0);
                }
                if (fabs(cond) <= 1e-9) break;

                if (env) {
                    env->curr_loop_iterations++;
                    if (env->curr_loop_iterations > env->max_loop_iterations) {
                        env->aborted = 1;
                        break;
                    }
                }
            }
            return last_val;
        }
        case EXPRTK_NODE_VECTOR: {
            size_t actual_count = 0;
            exprtk_value_t *vals = eval_expand_args(node->data.vector.elements, node->data.vector.count, env, &actual_count);
            if (!vals && actual_count == 0) return zero;
            if (actual_count == 0) { free(vals); return zero; }

            double *data = (double*)mem_alloc(&env->arena, actual_count * sizeof(double));
            for (size_t i = 0; i < actual_count; ++i) {
                data[i] = val_to_double(vals[i]);
            }
            free(vals);
            return exprtk_val_vec(data, actual_count);
        }
        case EXPRTK_NODE_INDEX: {
            exprtk_value_t arr = exprtk_eval(node->data.index_access.array, env);
            exprtk_value_t idx_val = exprtk_eval(node->data.index_access.index, env);

            if (arr.type == EXPRTK_VAL_VECTOR && (idx_val.type == EXPRTK_VAL_NUMBER || idx_val.type == EXPRTK_VAL_INTEGER)) {
                int idx = (idx_val.type == EXPRTK_VAL_INTEGER) ? (int)idx_val.data.integer : (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.vector.size) {
                    return throw_error(env, node, "Array index %d out of bounds [0, %zu)", idx, arr.data.vector.size);
                }
                return exprtk_val_num(arr.data.vector.data[idx]);
            }
            /* List index: l[i] → any value */
            if (arr.type == EXPRTK_VAL_LIST && (idx_val.type == EXPRTK_VAL_NUMBER || idx_val.type == EXPRTK_VAL_INTEGER)) {
                int idx = (idx_val.type == EXPRTK_VAL_INTEGER) ? (int)idx_val.data.integer : (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.list.count) {
                    return throw_error(env, node, "List index %d out of bounds [0, %zu)", idx, arr.data.list.count);
                }
                return arr.data.list.items[idx];
            }
            /* Dynamic map index: m["key"] */
            if (arr.type == EXPRTK_VAL_MAP && idx_val.type == EXPRTK_VAL_STRING) {
                return exprtk_map_get(&arr, idx_val.data.string.data);
            }

            // Type error
            return throw_error(env, node, "Invalid indexing: expected vector[number], list[number], or map[string], got %s[%s]",
                             type_name(arr.type), type_name(idx_val.type));
        }
        case EXPRTK_NODE_SLICE: {
            exprtk_value_t arr = exprtk_eval(node->data.slice.array, env);
            exprtk_value_t start_val = exprtk_eval(node->data.slice.start, env);
            exprtk_value_t end_val = exprtk_eval(node->data.slice.end, env);
            if (arr.type != EXPRTK_VAL_VECTOR ||
                (start_val.type != EXPRTK_VAL_NUMBER && start_val.type != EXPRTK_VAL_INTEGER) ||
                (end_val.type != EXPRTK_VAL_NUMBER && end_val.type != EXPRTK_VAL_INTEGER)) {
                return throw_error(env, node,
                                   "Invalid slice: expected vector[number:number], got %s[%s:%s]",
                                   type_name(arr.type), type_name(start_val.type), type_name(end_val.type));
            }
            int start = (start_val.type == EXPRTK_VAL_INTEGER) ? (int)start_val.data.integer : (int)start_val.data.number;
            int end = (end_val.type == EXPRTK_VAL_INTEGER) ? (int)end_val.data.integer : (int)end_val.data.number;
            if (start < 0) start = 0;
            if (end > (int)arr.data.vector.size) end = (int)arr.data.vector.size;
            if (start > end) return throw_error(env, node, "Invalid slice range [%d:%d]", start, end);
            size_t count = (size_t)(end - start);
            double *data = (double*)mem_alloc(node->arena, count * sizeof(double));
            if (!data) return throw_error(env, node, "Out of memory creating slice");
            for (size_t i = 0; i < count; ++i) {
                data[i] = arr.data.vector.data[start + i];
            }
            return exprtk_val_vec(data, count);
        }
        case EXPRTK_NODE_MAP_LITERAL: {
            exprtk_value_t map = exprtk_val_map();
            for (size_t i = 0; i < node->data.map_literal.count; ++i) {
                if (node->data.map_literal.keys[i] == NULL) {
                    /* Spread operator */
                    exprtk_value_t other = exprtk_eval(node->data.map_literal.values[i], env);
                    if (other.type == EXPRTK_VAL_MAP) {
                        exprtk_map_iter_t it = exprtk_map_iter_begin(&other);
                        const char *k;
                        exprtk_value_t v;
                        while (exprtk_map_iter_next(&it, &k, &v)) {
                            exprtk_map_set(&map, k, v);
                        }
                    }
                } else {
                    exprtk_value_t val = exprtk_eval(node->data.map_literal.values[i], env);
                    if (env && env->flow != exprtk_FLOW_NORMAL) return map;
                    exprtk_map_set(&map, node->data.map_literal.keys[i], val);
                }
            }
            return map;
        }
        case EXPRTK_NODE_MEMBER_ACCESS: {
            exprtk_value_t obj = exprtk_eval(node->data.member_access.object, env);
            const char *member = node->data.member_access.member;

            // OOP: Instance field/method access
            if (obj.type == EXPRTK_VAL_INSTANCE) {
                exprtk_instance_t *instance = obj.data.instance_val.instance;

                // Try field first
                exprtk_value_t field_val;
                if (exprtk_instance_get_field(instance, member, &field_val)) {
                    exprtk_class_t *field_owner = NULL;
                    int access = exprtk_class_get_instance_field_access(instance->klass, member,
                                                                        &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_access.object, member)) {
                        return throw_field_access_error(env, node, member, access);
                    }
                    return field_val;
                }

                // Try method
                exprtk_func_t *method = exprtk_instance_get_method(instance, member);
                if (method) {
                    if (!can_access_method(env, method, node->data.member_access.object)) {
                        return throw_method_access_error(env, node, member, method);
                    }
                    return exprtk_val_bound_method(instance, method);
                }

                return throw_error(env, node, "Instance has no field or method '%s'", member);
            }

            // OOP: Class static field/method access
            if (obj.type == EXPRTK_VAL_CLASS) {
                exprtk_class_t *klass = obj.data.class_val.klass;

                exprtk_value_t static_field;
                if (exprtk_class_get_static_field(klass, member, &static_field)) {
                    exprtk_class_t *field_owner = NULL;
                    int access = exprtk_class_get_static_field_access(klass, member,
                                                                      &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_access.object, member)) {
                        return throw_field_access_error(env, node, member, access);
                    }
                    return static_field;
                }

                exprtk_func_t *static_method = exprtk_class_lookup_method(klass, member, 1);
                if (static_method) {
                    if (!can_access_method(env, static_method, node->data.member_access.object)) {
                        return throw_method_access_error(env, node, member, static_method);
                    }
                    // For static methods, we don't bind 'this'
                    exprtk_value_t func_val;
                    func_val.type = EXPRTK_VAL_FUNCTION;
                    func_val.data.function.arg_params = static_method->data.script.arg_params;
                    func_val.data.function.arg_count = static_method->data.script.arg_count;
                    func_val.data.function.body = static_method->data.script.body;
                    func_val.data.function.closure_env = static_method->closure_env;
                    func_val.data.function.owner_class = static_method->owner_class;
                    func_val.data.function.is_static_method = static_method->is_static_method;
                    func_val.data.function.access_level = static_method->access_level;
                    return func_val;
                }
                return throw_error(env, node, "Class has no static field or method '%s'", member);
            }

            if (obj.type == EXPRTK_VAL_MAP) {
                return exprtk_map_get(&obj, member);
            }
            /* String property access */
            if (obj.type == EXPRTK_VAL_STRING) {
                if (strcmp(member, "length") == 0) return exprtk_val_num((double)obj.data.string.len);
            }
            /* Vector property access */
            if (obj.type == EXPRTK_VAL_VECTOR) {
                if (strcmp(member, "length") == 0) return exprtk_val_num((double)obj.data.vector.size);
            }
            return throw_error(env, node, "Member access '%s' is invalid for %s",
                               member ? member : "<null>", type_name(obj.type));
        }
        case EXPRTK_NODE_MEMBER_SET: {
            exprtk_node_t *object_node = node->data.member_set.object;
            if (!object_node ||
                (object_node->type != EXPRTK_NODE_VARIABLE &&
                 object_node->type != EXPRTK_NODE_THIS &&
                 object_node->type != EXPRTK_NODE_SUPER)) {
                return throw_error(env, node,
                                   "Member assignment '%s' requires a variable receiver, this, or static super",
                                   node->data.member_set.member ? node->data.member_set.member : "<null>");
            }

            if (object_node->type == EXPRTK_NODE_SUPER) {
                if (!object_node->data.super_expr.member ||
                    object_node->data.super_expr.is_call ||
                    !env) {
                    return throw_error(env, node,
                                       "super field assignment is only valid inside class methods");
                }

                exprtk_class_t *owner_class = eval_current_class(env);
                if (!owner_class) {
                    return throw_error(env, node, "super is only valid inside class methods");
                }

                exprtk_class_t *parent = owner_class->prototype;
                if (!parent) {
                    return throw_error(env, node, "Class '%s' has no parent class",
                                       owner_class->name ? owner_class->name : "<unknown>");
                }

                if (env->current_method_is_static) {
                    exprtk_class_t *field_owner = NULL;
                    int access = exprtk_class_get_static_field_access(
                        parent, node->data.member_set.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access, object_node,
                                                   node->data.member_set.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.member_set.member,
                                                        access);
                    }
                    exprtk_value_t val = exprtk_eval(node->data.member_set.value, env);
                    if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
                    exprtk_class_set_static_field(parent, node->data.member_set.member, val);
                    return val;
                }

                exprtk_value_t this_val = exprtk_env_get(env, "this");
                if (this_val.type != EXPRTK_VAL_INSTANCE) {
                    return throw_error(env, node, "super is only valid inside instance methods or constructors");
                }

                exprtk_value_t val = exprtk_eval(node->data.member_set.value, env);
                if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
                exprtk_class_t *field_owner = NULL;
                int access = exprtk_class_get_instance_field_access(
                    parent, node->data.member_set.member, &field_owner);
                if (!can_access_declared_field(env, field_owner, access, object_node,
                                               node->data.member_set.member)) {
                    return throw_field_access_error(env, node,
                                                    node->data.member_set.member,
                                                    access);
                }
                exprtk_instance_set_field(this_val.data.instance_val.instance,
                                          node->data.member_set.member, val);
                return val;
            }

            /* Evaluate the new value only after the receiver shape is accepted. */
            exprtk_value_t val = exprtk_eval(node->data.member_set.value, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
            if (object_node->type == EXPRTK_NODE_VARIABLE) {
                const char *var_name = object_node->data.variable.name;
                /* Get the variable from hash table */
                exprtk_value_t var_val = exprtk_env_get(env, var_name);
                if (var_val.type == EXPRTK_VAL_MAP) {
                    exprtk_map_set(&var_val, node->data.member_set.member, val);
                    /* Update the variable in hash table */
                    exprtk_env_set(env, var_name, var_val);
                    return val;
                }
                // OOP: Instance field assignment
                if (var_val.type == EXPRTK_VAL_INSTANCE) {
                    exprtk_instance_t *instance = var_val.data.instance_val.instance;
                    exprtk_class_t *field_owner = NULL;
                    int access = exprtk_class_get_instance_field_access(
                        instance->klass, node->data.member_set.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_set.object,
                                                   node->data.member_set.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.member_set.member,
                                                        access);
                    }

                    exprtk_instance_set_field(instance, node->data.member_set.member, val);
                    return val;
                }
                if (var_val.type == EXPRTK_VAL_CLASS) {
                    exprtk_class_t *klass = var_val.data.class_val.klass;
                    exprtk_class_t *field_owner = NULL;
                    int access = exprtk_class_get_static_field_access(
                        klass, node->data.member_set.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_set.object,
                                                   node->data.member_set.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.member_set.member,
                                                        access);
                    }

                    exprtk_class_set_static_field(klass, node->data.member_set.member, val);
                    return val;
                }
            }

            if (object_node->type == EXPRTK_NODE_THIS) {
                exprtk_value_t this_val = exprtk_eval(object_node, env);
                if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
                if (this_val.type != EXPRTK_VAL_INSTANCE) {
                    return throw_error(env, node, "this is only valid inside instance methods or constructors");
                }

                exprtk_instance_t *instance = this_val.data.instance_val.instance;
                exprtk_class_t *field_owner = NULL;
                int access = exprtk_class_get_instance_field_access(
                    instance->klass, node->data.member_set.member, &field_owner);
                if (!can_access_declared_field(env, field_owner, access, object_node,
                                               node->data.member_set.member)) {
                    return throw_field_access_error(env, node,
                                                    node->data.member_set.member,
                                                    access);
                }

                exprtk_instance_set_field(instance, node->data.member_set.member, val);
                return val;
            }

            return throw_error(env, node, "Member assignment '%s' requires a map, instance, or class",
                               node->data.member_set.member ? node->data.member_set.member : "<null>");
        }
        case EXPRTK_NODE_NULL: {
            exprtk_value_t null_val;
            memset(&null_val, 0, sizeof(null_val));
            null_val.type = EXPRTK_VAL_NULL;
            return null_val;
        }
        case EXPRTK_NODE_FOR_IN: {
            exprtk_value_t collection = exprtk_eval(node->data.for_in.collection, env);
            if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) return zero;
            exprtk_value_t last_val = zero;

            if (collection.type == EXPRTK_VAL_VECTOR) {
                for (size_t i = 0; i < collection.data.vector.size; ++i) {
                    if (env) {
                        env->curr_loop_iterations++;
                        if (env->curr_loop_iterations > env->max_loop_iterations) { env->aborted = 1; break; }
                    }
                    exprtk_env_set(env, node->data.for_in.var_name, exprtk_val_num(collection.data.vector.data[i]));
                    last_val = exprtk_eval(node->data.for_in.body, env);
                    if (env && env->flow == exprtk_FLOW_BREAK) { env->flow = exprtk_FLOW_NORMAL; break; }
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                    if (env && env->flow == exprtk_FLOW_RETURN) break;
                }
            } else if (collection.type == EXPRTK_VAL_MAP) {
                exprtk_map_iter_t it = exprtk_map_iter_begin(&collection);
                const char *key;
                while (exprtk_map_iter_next(&it, &key, NULL)) {
                    if (env) {
                        env->curr_loop_iterations++;
                        if (env->curr_loop_iterations > env->max_loop_iterations) { env->aborted = 1; break; }
                    }
                    tstr_v sv;
                    sv.data = (char*)key;
                    sv.len = strlen(key);
                    exprtk_env_set(env, node->data.for_in.var_name, exprtk_val_str(sv));
                    last_val = exprtk_eval(node->data.for_in.body, env);
                    if (env && env->flow == exprtk_FLOW_BREAK) { env->flow = exprtk_FLOW_NORMAL; break; }
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                    if (env && env->flow == exprtk_FLOW_RETURN) break;
                }
            } else if (collection.type == EXPRTK_VAL_LIST) {
                for (size_t i = 0; i < collection.data.list.count; ++i) {
                    if (env) {
                        env->curr_loop_iterations++;
                        if (env->curr_loop_iterations > env->max_loop_iterations) { env->aborted = 1; break; }
                    }
                    exprtk_env_set(env, node->data.for_in.var_name, collection.data.list.items[i]);
                    last_val = exprtk_eval(node->data.for_in.body, env);
                    if (env && env->flow == exprtk_FLOW_BREAK) { env->flow = exprtk_FLOW_NORMAL; break; }
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                    if (env && env->flow == exprtk_FLOW_RETURN) break;
                }
            }
            return last_val;
        }

        case EXPRTK_NODE_THROW: {
            exprtk_value_t thrown = zero;
            if (node->data.throw_stmt.value) {
                thrown = exprtk_eval(node->data.throw_stmt.value, env);
            }
            if (env) {
                env->flow = exprtk_FLOW_THROW;
                env->error_value = thrown;
            }
            return thrown;
        }

        case EXPRTK_NODE_TRY_CATCH: {
            /* Evaluate try body */
            exprtk_value_t result = exprtk_eval(node->data.try_catch.try_body, env);

            /* If a throw occurred, handle catch */
            if (env && env->flow == exprtk_FLOW_THROW) {
                env->flow = exprtk_FLOW_NORMAL;

                exprtk_env_t catch_env;
                exprtk_env_init_local(&catch_env);
                catch_env.parent = env;
                catch_env.eval_node = env->eval_node;
                catch_env.exec_script_body = env->exec_script_body;
                catch_env.max_recursion = env->max_recursion;
                catch_env.curr_recursion = env->curr_recursion;
                catch_env.max_loop_iterations = env->max_loop_iterations;
                catch_env.curr_loop_iterations = env->curr_loop_iterations;
                catch_env.max_nodes = env->max_nodes;
                catch_env.curr_nodes = env->curr_nodes;

                /* Bind error value to catch variable if one was declared */
                if (node->data.try_catch.catch_var) {
                    exprtk_env_set_local(&catch_env, node->data.try_catch.catch_var, env->error_value);
                }

                /* Evaluate catch body */
                result = exprtk_eval(node->data.try_catch.catch_body, &catch_env);
                result = exprtk_value_clone_to_env(result, env);

                env->curr_nodes = catch_env.curr_nodes;
                env->curr_loop_iterations = catch_env.curr_loop_iterations;
                env->aborted = catch_env.aborted;
                if (catch_env.flow != exprtk_FLOW_NORMAL) {
                    env->flow = catch_env.flow;
                    env->return_value = exprtk_value_clone_to_env(catch_env.return_value, env);
                    env->error_value = exprtk_value_clone_to_env(catch_env.error_value, env);
                }

                exprtk_env_free(&catch_env);
            }

            return result;
        }

        case EXPRTK_NODE_FUNCTION_EXPRESSION: {
            /* Create a function value that captures the current environment */
            exprtk_value_t val;
            val.type = EXPRTK_VAL_FUNCTION;
            val.data.function.arg_params = node->data.func_def.arg_params;
            val.data.function.arg_count  = node->data.func_def.arg_count;
            val.data.function.body       = node->data.func_def.body;
            val.data.function.closure_env = exprtk_env_snapshot(env);  /* capture enclosing scope */
            val.data.function.owner_class = NULL;
            val.data.function.is_static_method = 0;
            val.data.function.access_level = EXPRTK_ACCESS_PUBLIC;
            return val;
        }

        case EXPRTK_NODE_SWITCH: {
            exprtk_value_t switch_val = exprtk_eval(node->data.switch_stmt.value, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;

            for (size_t i = 0; i < node->data.switch_stmt.case_count; ++i) {
                exprtk_value_t case_val = exprtk_eval(node->data.switch_stmt.cases[i * 2], env);
                if (env && env->flow != exprtk_FLOW_NORMAL) return zero;

                int match = 0;
                if ((switch_val.type == EXPRTK_VAL_NUMBER || switch_val.type == EXPRTK_VAL_INTEGER ||
                     switch_val.type == EXPRTK_VAL_STRING || switch_val.type == EXPRTK_VAL_NULL) &&
                    (case_val.type == EXPRTK_VAL_NUMBER || case_val.type == EXPRTK_VAL_INTEGER ||
                     case_val.type == EXPRTK_VAL_STRING || case_val.type == EXPRTK_VAL_NULL))
                    match = values_match(switch_val, case_val);

                if (match)
                    return exprtk_eval(node->data.switch_stmt.cases[i * 2 + 1], env);
            }

            if (node->data.switch_stmt.default_case)
                return exprtk_eval(node->data.switch_stmt.default_case, env);

            return zero;
        }

        // ====================================================================
        // OOP (Object-Oriented Programming) Node Evaluation
        // ====================================================================

        case EXPRTK_NODE_CLASS_DEF: {
            return eval_class_def_node(node, env);
        }

        case EXPRTK_NODE_THIS: {
            // 'this' keyword - lookup in environment
            if (env && env->current_method_is_static) {
                return throw_error(env, node, "this is only valid inside instance methods or constructors");
            }

            exprtk_value_t this_val = exprtk_env_get(env, "this");
            if (this_val.type != EXPRTK_VAL_INSTANCE) {
                return throw_error(env, node, "this is only valid inside instance methods or constructors");
            }
            return this_val;
        }

        case EXPRTK_NODE_SUPER: {
            // super() or super.method
            exprtk_class_t *owner_class = eval_current_class(env);
            if (env && env->current_method_is_static) {
                if (!owner_class) {
                    return throw_error(env, node, "super is only valid inside class methods");
                }

                exprtk_class_t *parent = owner_class->prototype;
                if (!parent) {
                    return throw_error(env, node, "Class '%s' has no parent class",
                                       owner_class->name ? owner_class->name : "<unknown>");
                }

                if (node->data.super_expr.member == NULL) {
                    return throw_error(env, node, "super() is only valid inside instance constructors");
                }

                if (node->data.super_expr.is_call) {
                    size_t actual_count = 0;
                    exprtk_value_t *args = eval_expand_args(node->data.super_expr.args,
                                                            node->data.super_expr.arg_count,
                                                            env, &actual_count);
                    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
                        free(args);
                        return zero;
                    }

                    exprtk_func_t *method = exprtk_class_lookup_method_typed(
                        parent, node->data.super_expr.member, 1, actual_count, args);
                    if (!method) {
                        free(args);
                        return throw_error(env, node, "Parent class has no static method '%s'",
                                           node->data.super_expr.member);
                    }
                    if (!can_access_method(env, method, node)) {
                        free(args);
                        return throw_method_access_error(env, node,
                                                         node->data.super_expr.member,
                                                         method);
                    }

                    exprtk_value_t result = eval_script_function(method, actual_count, args, env, env);
                    free(args);
                    return result;
                }

                exprtk_value_t static_field;
                if (exprtk_class_get_static_field(parent, node->data.super_expr.member,
                                                  &static_field)) {
                    exprtk_class_t *field_owner = NULL;
                    int access = exprtk_class_get_static_field_access(
                        parent, node->data.super_expr.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access, node,
                                                   node->data.super_expr.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.super_expr.member,
                                                        access);
                    }
                    return static_field;
                }

                exprtk_func_t *method =
                    exprtk_class_lookup_method(parent, node->data.super_expr.member, 1);
                if (!method) {
                    return throw_error(env, node, "Parent class has no static field or method '%s'",
                                       node->data.super_expr.member);
                }
                if (!can_access_method(env, method, node)) {
                    return throw_method_access_error(env, node, node->data.super_expr.member,
                                                     method);
                }

                exprtk_value_t func_val;
                func_val.type = EXPRTK_VAL_FUNCTION;
                func_val.data.function.arg_params = method->data.script.arg_params;
                func_val.data.function.arg_count = method->data.script.arg_count;
                func_val.data.function.body = method->data.script.body;
                func_val.data.function.closure_env = method->closure_env;
                func_val.data.function.owner_class = method->owner_class;
                func_val.data.function.is_static_method = method->is_static_method;
                func_val.data.function.access_level = method->access_level;
                return func_val;
            }

            // Get 'this' from environment
            exprtk_value_t this_val = exprtk_env_get(env, "this");
            if (this_val.type != EXPRTK_VAL_INSTANCE) {
                return throw_error(env, node, "super is only valid inside instance methods or constructors");
            }

            exprtk_instance_t *instance = this_val.data.instance_val.instance;
            exprtk_class_t *parent = owner_class ? owner_class->prototype : instance->klass->prototype;
            if (!parent) {
                const char *owner_name = owner_class ? owner_class->name : instance->klass->name;
                return throw_error(env, node, "Class '%s' has no parent class",
                                   owner_name ? owner_name : "<unknown>");
            }

            if (node->data.super_expr.member == NULL) {
                if (!env || !env->current_method_is_constructor) {
                    return throw_error(env, node, "super() is only valid inside instance constructors");
                }

                size_t actual_count = 0;
                exprtk_value_t *args = eval_expand_args(node->data.super_expr.args,
                                                        node->data.super_expr.arg_count,
                                                        env, &actual_count);
                if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
                    free(args);
                    return zero;
                }

                exprtk_func_t *constructor =
                    eval_find_constructor_typed(parent, actual_count, args);
                if (!constructor) {
                    free(args);
                    return zero;
                }

                exprtk_value_t result =
                    eval_script_function(constructor, actual_count, args, env, env);
                free(args);
                return result;
            } else {
                if (node->data.super_expr.is_call) {
                    size_t actual_count = 0;
                    exprtk_value_t *args = eval_expand_args(node->data.super_expr.args,
                                                            node->data.super_expr.arg_count,
                                                            env, &actual_count);
                    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
                        free(args);
                        return zero;
                    }

                    // super.method(...) - lookup method in parent class
                    exprtk_func_t *method = exprtk_class_lookup_method_typed(
                        parent, node->data.super_expr.member, 0, actual_count, args);
                    if (!method) {
                        free(args);
                        return throw_error(env, node, "Parent class has no method '%s'",
                                           node->data.super_expr.member);
                    }
                    if (!can_access_method(env, method, node)) {
                        free(args);
                        return throw_method_access_error(env, node,
                                                         node->data.super_expr.member,
                                                         method);
                    }

                    exprtk_value_t result = eval_script_function(method, actual_count, args, env, env);
                    free(args);
                    return result;
                }

                // super.method - lookup method in parent class
                exprtk_func_t *method = exprtk_class_lookup_method(parent, node->data.super_expr.member, 0);
                if (method) {
                    if (!can_access_method(env, method, node)) {
                        return throw_method_access_error(env, node,
                                                         node->data.super_expr.member,
                                                         method);
                    }
                    // Return bound method
                    return exprtk_val_bound_method(instance, method);
                }

                exprtk_value_t field_value;
                if (exprtk_instance_get_field(instance, node->data.super_expr.member, &field_value)) {
                    exprtk_class_t *field_owner = NULL;
                    int access = exprtk_class_get_instance_field_access(
                        parent, node->data.super_expr.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access, node,
                                                   node->data.super_expr.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.super_expr.member,
                                                        access);
                    }
                    return field_value;
                }

                return throw_error(env, node, "Parent class has no method or instance field '%s'",
                                   node->data.super_expr.member);
            }
        }

        case EXPRTK_NODE_INSTANCEOF: {
            // obj instanceof ClassName
            exprtk_value_t obj = exprtk_eval(node->data.instanceof_expr.object, env);
            if (env->flow != exprtk_FLOW_NORMAL) return zero;

            if (obj.type != EXPRTK_VAL_INSTANCE) {
                return exprtk_val_num(0.0); // Not an instance
            }

            exprtk_class_t *klass = NULL;
            exprtk_value_t class_value;
            if (node->data.instanceof_expr.class_expr) {
                class_value = exprtk_eval(node->data.instanceof_expr.class_expr, env);
                if (env->flow != exprtk_FLOW_NORMAL) return zero;
            } else {
                class_value = exprtk_env_get(env, node->data.instanceof_expr.class_name);
            }
            if (class_value.type == EXPRTK_VAL_CLASS)
                klass = class_value.data.class_val.klass;
            if (!klass) return exprtk_val_num(0.0);

            int is_instance = exprtk_instance_of(obj.data.instance_val.instance, klass);
            return exprtk_val_num(is_instance ? 1.0 : 0.0);
        }

        default:
            return zero;
    }
}
