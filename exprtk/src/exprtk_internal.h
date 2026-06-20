/**
 * @file exprtk_internal.h
 * @brief Internal shared header for exprtk modules
 *
 * This header is used by the split source files (exprtk_core.c,
 * exprtk_math.c, exprtk_finance.c, exprtk_ta.c, exprtk_registry.c,
 * and exprtk_mod_*.c modules) to share internal types, helpers, and
 * declarations.
 */

#ifndef EXPRTK_INTERNAL_H
#define EXPRTK_INTERNAL_H

#include "exprtk.h"
#include "exprtk_class.h"
#include "exprtk_module.h"
#include "exprtk_types.h"
#include "simd_helpers.h"
#include "turbo_buffer.h"


#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Internal registry */
void exprtk_registry_init(void);
exprtk_builtin_fn exprtk_registry_find(const char *name);
exprtk_value_t exprtk_value_clone_to_env(exprtk_value_t value, exprtk_env_t *dst_env);
exprtk_value_t exprtk_env_eval_node(const exprtk_node_t *node, exprtk_env_t *env);
exprtk_env_t *exprtk_env_snapshot(exprtk_env_t *env);
void exprtk_env_import_vars(exprtk_env_t *dst, exprtk_env_t *src);
void exprtk_env_set_local_borrowed(exprtk_env_t *env, const char *name,
                                   exprtk_value_t value);
exprtk_value_t throw_error(exprtk_env_t *env, const exprtk_node_t *node, const char *fmt, ...);
exprtk_value_t throw_method_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                         const char *method_name,
                                         exprtk_func_t *method);
exprtk_value_t throw_field_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                        const char *field_name, int access_level);
const char *type_name(int type);
double val_to_double(exprtk_value_t val);
int values_match(exprtk_value_t lhs, exprtk_value_t rhs);
exprtk_value_t eval_script_function(exprtk_func_t *func, size_t argc,
                                    exprtk_value_t *args, exprtk_env_t *parent_env,
                                    exprtk_env_t *caller_env);
exprtk_value_t *eval_expand_args(exprtk_node_t **nodes, size_t count,
                                 exprtk_env_t *env, size_t *out_count);
exprtk_class_t *eval_current_class(exprtk_env_t *env);
exprtk_func_t *eval_find_constructor_typed(exprtk_class_t *klass, size_t argc,
                                           exprtk_value_t *args);
exprtk_value_t eval_class_value_instantiation(exprtk_class_t *klass,
                                              const char *class_name,
                                              exprtk_node_t **arg_nodes,
                                              size_t arg_count,
                                              exprtk_env_t *env);
exprtk_value_t eval_class_instantiation(const char *class_name,
                                        exprtk_node_t **arg_nodes, size_t arg_count,
                                        exprtk_env_t *env);
exprtk_value_t eval_instance_script_method(exprtk_instance_t *instance,
                                           exprtk_func_t *method, size_t argc,
                                           exprtk_value_t *args, exprtk_env_t *env);
exprtk_value_t eval_class_def_node(const exprtk_node_t *node, exprtk_env_t *env);
int can_access_method(exprtk_env_t *env, exprtk_func_t *method,
                      const exprtk_node_t *object_node);
int can_access_declared_field(exprtk_env_t *env, exprtk_class_t *owner_class,
                              int access_level,
                              const exprtk_node_t *object_node,
                              const char *field_name);

typedef struct {
    const char      *method;
    exprtk_value_t   obj;
    exprtk_value_t  *args;
    size_t           argc;
    exprtk_node_t   *obj_node;
    exprtk_env_t    *env;
    mem_pool_t      *arena;
} mc_ctx_t;

exprtk_value_t eval_list_method(mc_ctx_t *mc);
exprtk_value_t eval_map_method(mc_ctx_t *mc);
exprtk_value_t eval_string_method(mc_ctx_t *mc);
exprtk_value_t eval_vector_method(mc_ctx_t *mc);

/* Built-in module accessors */
const exprtk_module_t *exprtk_module_math(void);
const exprtk_module_t *exprtk_module_string(void);
const exprtk_module_t *exprtk_module_stats(void);
const exprtk_module_t *exprtk_module_io(void);
const exprtk_module_t *exprtk_module_core(void);
const exprtk_module_t *exprtk_module_regex(void);

void exprtk_regex_ctx_destroy(void *ctx);

#endif /* EXPRTK_INTERNAL_H */
