/**
 * @file mod_core.c
 * @brief Core language utility module: typeof, type predicates, range.
 *
 * These are universal functions that don't belong to math/string/stats
 * but are fundamental to the language itself.
 */

#include "exprtk_module.h"
#include "exprtk_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int exprtk_is_numeric(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_NUMBER || value.type == EXPRTK_VAL_INTEGER;
}

static double exprtk_numeric_value(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_INTEGER) return (double)value.data.integer;
    if (value.type == EXPRTK_VAL_NUMBER) return value.data.number;
    return 0.0;
}

static exprtk_value_t exprtk_call_callable(exprtk_value_t callable, size_t argc, exprtk_value_t *args,
                                           exprtk_env_t *env, mem_pool_t *arena) {
    static const char *temp_name = "__ts_pipeline_fn";

    if (!env) return exprtk_val_num(0);

    if (callable.type == EXPRTK_VAL_STRING) {
        char fn_name[256];
        size_t len = callable.data.string.len < sizeof(fn_name) - 1 ? callable.data.string.len
                                                                    : sizeof(fn_name) - 1;
        memcpy(fn_name, callable.data.string.data, len);
        fn_name[len] = '\0';
        return exprtk_call_internal(fn_name, argc, args, env, arena);
    }

    if (callable.type == EXPRTK_VAL_FUNCTION) {
        exprtk_env_set_local(env, temp_name, callable);
        return exprtk_call_internal(temp_name, argc, args, env, arena);
    }

    return exprtk_val_num(0);
}

/* =========================================================================
 * 1. typeof(x) → string
 * ========================================================================= */

static exprtk_value_t fn_typeof(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    const char *name = "null";
    size_t len = 4;

    if (argc > 0) {
        switch (args[0].type) {
            case EXPRTK_VAL_NUMBER: name = "number"; len = 6; break;
            case EXPRTK_VAL_INTEGER: name = "number"; len = 6; break;
            case EXPRTK_VAL_STRING: name = "string"; len = 6; break;
            case EXPRTK_VAL_VECTOR: name = "vector"; len = 6; break;
            case EXPRTK_VAL_MAP:    name = "map";    len = 3; break;
            case EXPRTK_VAL_NULL:   name = "null";   len = 4; break;
            case EXPRTK_VAL_LIST:   name = "list";   len = 4; break;
            case EXPRTK_VAL_FUNCTION: name = "function"; len = 8; break;
            case EXPRTK_VAL_CLASS:  name = "class";  len = 5; break;
            case EXPRTK_VAL_INSTANCE: name = "instance"; len = 8; break;
            case EXPRTK_VAL_BOUND_METHOD: name = "function"; len = 8; break;
            default:                name = "unknown"; len = 7; break;
        }
    }

    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, name, len);
    buf[len] = '\0';
    tstr_v sv;
    sv.data = buf;
    sv.len = len;
    return exprtk_val_str(sv);
}

/* =========================================================================
 * 2. Type predicates: is_number, is_string, is_vector, is_map, is_null
 * ========================================================================= */

static exprtk_value_t fn_is_number(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 &&
                          (args[0].type == EXPRTK_VAL_NUMBER ||
                           args[0].type == EXPRTK_VAL_INTEGER) ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_string(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_STRING ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_vector(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_VECTOR ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_map(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_MAP ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_null(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_NULL ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_list(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_LIST ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_function(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 &&
                          (args[0].type == EXPRTK_VAL_FUNCTION ||
                           args[0].type == EXPRTK_VAL_BOUND_METHOD) ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_class(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_CLASS ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_instance(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_INSTANCE ? 1.0 : 0.0);
}

static int exprtk_string_equals(exprtk_value_t value, const char *text) {
    size_t len = text ? strlen(text) : 0;
    return value.type == EXPRTK_VAL_STRING &&
           value.data.string.len == len &&
           value.data.string.data &&
           memcmp(value.data.string.data, text, len) == 0;
}

static exprtk_value_t fn_await(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc < 1) return exprtk_val_num(0);

    if (args[0].type == EXPRTK_VAL_MAP && exprtk_map_has(&args[0], "status") &&
        exprtk_map_has(&args[0], "value")) {
        exprtk_value_t status = exprtk_map_get(&args[0], "status");
        if (exprtk_string_equals(status, "suspended") ||
            exprtk_string_equals(status, "dead")) {
            return exprtk_map_get(&args[0], "value");
        }
    }

    return args[0];
}

/* =========================================================================
 * 3. range(start, end [, step]) → vector
 * ========================================================================= */

static exprtk_value_t fn_range(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc < 2) return exprtk_val_num(0);
    if (!exprtk_is_numeric(args[0]) || !exprtk_is_numeric(args[1])) return exprtk_val_num(0);

    double start = exprtk_numeric_value(args[0]);
    double end   = exprtk_numeric_value(args[1]);
    double step  = 1.0;

    if (argc >= 3) {
        if (!exprtk_is_numeric(args[2])) return exprtk_val_num(0);
        step = exprtk_numeric_value(args[2]);
        if (fabs(step) < 1e-15) return exprtk_val_num(0);
    }

    /* Validate direction */
    if ((end > start && step < 0) || (end < start && step > 0))
        return exprtk_val_num(0);

    /* Count elements */
    size_t n = 0;
    n = (size_t)(fabs((end - start) / step)) + 1;
    if (n > 100000) n = 100000; /* safety cap */

    double *data = MEM_ALLOC_ARRAY(arena, double, n);
    if (!data) return exprtk_val_num(0);

    for (size_t i = 0; i < n; ++i) {
        data[i] = start + (double)i * step;
    }
    return exprtk_val_vec(data, n);
}

/* =========================================================================
 * 4. print(...) → prints to stdout, returns last arg
 * ========================================================================= */

static exprtk_value_t fn_print(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    for (size_t i = 0; i < argc; ++i) {
        if (i > 0) printf(" ");
        switch (args[i].type) {
            case EXPRTK_VAL_NUMBER: printf("%g", args[i].data.number); break;
            case EXPRTK_VAL_INTEGER: printf("%lld", (long long)args[i].data.integer); break;
            case EXPRTK_VAL_STRING: printf("%.*s", (int)args[i].data.string.len, args[i].data.string.data); break;
            case EXPRTK_VAL_VECTOR:
                printf("[");
                for (size_t j = 0; j < args[i].data.vector.size; ++j) {
                    if (j > 0) printf(", ");
                    printf("%g", args[i].data.vector.data[j]);
                    if (j >= 9 && args[i].data.vector.size > 10) { printf(", ...(%zu more)", args[i].data.vector.size - 10); break; }
                }
                printf("]");
                break;
            case EXPRTK_VAL_MAP:   printf("{map:%zu}", exprtk_map_count(&args[i])); break;
            case EXPRTK_VAL_NULL:  printf("null"); break;
            default:               printf("?"); break;
        }
    }
    printf("\n");
    return (argc > 0) ? args[argc - 1] : exprtk_val_num(0);
}

/* =========================================================================
 * 5. assert(cond [, msg]) → aborts if cond is falsy
 * ========================================================================= */

static exprtk_value_t fn_assert(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc < 1) return exprtk_val_num(0);
    double cond = exprtk_is_numeric(args[0]) ? exprtk_numeric_value(args[0]) : 0;
    if (fabs(cond) < 1e-9) {
        if (argc >= 2 && args[1].type == EXPRTK_VAL_STRING) {
            fprintf(stderr, "Assertion failed: %.*s\n", (int)args[1].data.string.len, args[1].data.string.data);
        } else {
            fprintf(stderr, "Assertion failed\n");
        }
        if (env) env->aborted = 1;
        return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

/* =========================================================================
 * 6. list(...) → creates a heterogeneous list from arguments
 * ========================================================================= */

static exprtk_value_t fn_list(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_value_t result = exprtk_val_list_empty();
    for (size_t i = 0; i < argc; ++i) {
        exprtk_list_push(&result, args[i]);
    }
    return result;
}

/* =========================================================================
 * 7. Pipeline Primitives (vector-first)
 * ========================================================================= */

static exprtk_value_t fn_map(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, mem_pool_t *arena) {
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR &&
        (args[1].type == EXPRTK_VAL_STRING || args[1].type == EXPRTK_VAL_FUNCTION)) {
        size_t n = args[0].data.vector.size;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        if (!out) return exprtk_val_num(0);

        for (size_t i = 0; i < n; ++i) {
            exprtk_value_t in = exprtk_val_num(args[0].data.vector.data[i]);
            exprtk_value_t mapped = exprtk_call_callable(args[1], 1, &in, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            out[i] = exprtk_numeric_value(mapped);
        }
        return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_filter(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR &&
        (args[1].type == EXPRTK_VAL_STRING || args[1].type == EXPRTK_VAL_FUNCTION)) {
        size_t n = args[0].data.vector.size;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        size_t kept = 0;
        if (!out && n > 0) return exprtk_val_num(0);

        for (size_t i = 0; i < n; ++i) {
            exprtk_value_t in = exprtk_val_num(args[0].data.vector.data[i]);
            exprtk_value_t pred = exprtk_call_callable(args[1], 1, &in, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            if (fabs(exprtk_numeric_value(pred)) > 1e-9) {
                out[kept++] = args[0].data.vector.data[i];
            }
        }
        return exprtk_val_vec(out, kept);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_reduce(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR &&
        (args[2].type == EXPRTK_VAL_STRING || args[2].type == EXPRTK_VAL_FUNCTION)) {
        exprtk_value_t acc = args[1];
        exprtk_value_t call_args[2];

        for (size_t i = 0; i < args[0].data.vector.size; ++i) {
            call_args[0] = acc;
            call_args[1] = exprtk_val_num(args[0].data.vector.data[i]);
            acc = exprtk_call_callable(args[2], 2, call_args, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
        }
        return acc;
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_take(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && exprtk_is_numeric(args[1])) {
        size_t n = args[0].data.vector.size;
        int count = (int)exprtk_numeric_value(args[1]);
        size_t out_n = 0;
        double *out = NULL;

        if (count < 0) count = 0;
        out_n = (size_t)count < n ? (size_t)count : n;
        out = MEM_ALLOC_ARRAY(arena, double, out_n);
        if (!out && out_n > 0) return exprtk_val_num(0);
        if (out_n > 0) memcpy(out, args[0].data.vector.data, out_n * sizeof(double));
        return exprtk_val_vec(out, out_n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_drop(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && exprtk_is_numeric(args[1])) {
        size_t n = args[0].data.vector.size;
        int count = (int)exprtk_numeric_value(args[1]);
        size_t start = 0;
        size_t out_n = 0;
        double *out = NULL;

        if (count < 0) count = 0;
        start = (size_t)count < n ? (size_t)count : n;
        out_n = n - start;
        out = MEM_ALLOC_ARRAY(arena, double, out_n);
        if (!out && out_n > 0) return exprtk_val_num(0);
        if (out_n > 0) memcpy(out, args[0].data.vector.data + start, out_n * sizeof(double));
        return exprtk_val_vec(out, out_n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_lag(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_VECTOR && exprtk_is_numeric(args[1])) {
        size_t n = args[0].data.vector.size;
        int shift = (int)exprtk_numeric_value(args[1]);
        double fill = (argc == 3 && exprtk_is_numeric(args[2])) ? exprtk_numeric_value(args[2]) : 0.0;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        if (!out && n > 0) return exprtk_val_num(0);

        for (size_t i = 0; i < n; ++i) {
            int src = (int)i - shift;
            out[i] = (src >= 0 && src < (int)n) ? args[0].data.vector.data[src] : fill;
        }
        return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static int table_key_from_value(exprtk_value_t value, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    if (value.type == EXPRTK_VAL_STRING) {
        size_t n = value.data.string.len < cap - 1 ? value.data.string.len : cap - 1;
        memcpy(buf, value.data.string.data, n);
        buf[n] = '\0';
        return 1;
    }
    if (exprtk_is_numeric(value)) {
        snprintf(buf, cap, "%.17g", exprtk_numeric_value(value));
        return 1;
    }
    return 0;
}

static int table_arg_key(exprtk_value_t value, char *buf, size_t cap) {
    if (value.type != EXPRTK_VAL_STRING || !buf || cap == 0) return 0;
    size_t n = value.data.string.len < cap - 1 ? value.data.string.len : cap - 1;
    memcpy(buf, value.data.string.data, n);
    buf[n] = '\0';
    return 1;
}

static exprtk_value_t table_clone_map(exprtk_value_t row) {
    exprtk_value_t out = exprtk_val_map();
    if (row.type != EXPRTK_VAL_MAP) return out;
    exprtk_map_iter_t it = exprtk_map_iter_begin(&row);
    const char *key = NULL;
    exprtk_value_t value;
    while (exprtk_map_iter_next(&it, &key, &value)) {
        exprtk_map_set(&out, key, value);
    }
    return out;
}

static exprtk_value_t fn_table_select(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc < 2 || args[0].type != EXPRTK_VAL_LIST) return exprtk_val_num(0);
    exprtk_value_t out = exprtk_val_list_empty();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t row = args[0].data.list.items[i];
        if (row.type != EXPRTK_VAL_MAP) continue;
        exprtk_value_t selected = exprtk_val_map();
        if (argc == 2 && args[1].type == EXPRTK_VAL_LIST) {
            for (size_t f = 0; f < args[1].data.list.count; ++f) {
                char key[128];
                if (table_arg_key(args[1].data.list.items[f], key, sizeof(key)) &&
                    exprtk_map_has(&row, key))
                    exprtk_map_set(&selected, key, exprtk_map_get(&row, key));
            }
        } else {
            for (size_t f = 1; f < argc; ++f) {
                char key[128];
                if (table_arg_key(args[f], key, sizeof(key)) && exprtk_map_has(&row, key))
                    exprtk_map_set(&selected, key, exprtk_map_get(&row, key));
            }
        }
        exprtk_list_push(&out, selected);
    }
    return out;
}

static exprtk_value_t fn_table_filter(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    if (argc != 2 || args[0].type != EXPRTK_VAL_LIST ||
        (args[1].type != EXPRTK_VAL_STRING && args[1].type != EXPRTK_VAL_FUNCTION))
        return exprtk_val_num(0);
    exprtk_value_t out = exprtk_val_list_empty();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t row = args[0].data.list.items[i];
        exprtk_value_t pred = exprtk_call_callable(args[1], 1, &row, env, arena);
        if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
        if (fabs(exprtk_numeric_value(pred)) > 1e-9)
            exprtk_list_push(&out, table_clone_map(row));
    }
    return out;
}

static int table_op_equals(exprtk_value_t value, const char *text) {
    return value.type == EXPRTK_VAL_STRING && value.data.string.len == strlen(text) &&
           memcmp(value.data.string.data, text, value.data.string.len) == 0;
}

static exprtk_value_t fn_table_groupby(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if ((argc != 3 && argc != 4) || args[0].type != EXPRTK_VAL_LIST)
        return exprtk_val_num(0);
    char key_field[128];
    char value_field[128];
    if (!table_arg_key(args[1], key_field, sizeof(key_field)) ||
        !table_arg_key(args[2], value_field, sizeof(value_field)))
        return exprtk_val_num(0);
    const char *op = "sum";
    if (argc == 4) {
        if (table_op_equals(args[3], "count")) op = "count";
        else if (table_op_equals(args[3], "mean")) op = "mean";
        else if (table_op_equals(args[3], "sum")) op = "sum";
        else return exprtk_val_num(0);
    }
    exprtk_value_t groups = exprtk_val_map();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t row = args[0].data.list.items[i];
        if (row.type != EXPRTK_VAL_MAP || !exprtk_map_has(&row, key_field)) continue;
        char group_key[128];
        if (!table_key_from_value(exprtk_map_get(&row, key_field), group_key, sizeof(group_key)))
            continue;
        exprtk_value_t acc = exprtk_map_has(&groups, group_key) ? exprtk_map_get(&groups, group_key)
                                                                : exprtk_val_map();
        double sum = exprtk_map_has(&acc, "sum") ? exprtk_numeric_value(exprtk_map_get(&acc, "sum")) : 0.0;
        double count = exprtk_map_has(&acc, "count") ? exprtk_numeric_value(exprtk_map_get(&acc, "count")) : 0.0;
        double v = 0.0;
        if (exprtk_map_has(&row, value_field))
            v = exprtk_numeric_value(exprtk_map_get(&row, value_field));
        sum += v;
        count += 1.0;
        exprtk_map_set(&acc, "sum", exprtk_val_num(sum));
        exprtk_map_set(&acc, "count", exprtk_val_num(count));
        exprtk_map_set(&acc, "value",
                       exprtk_val_num(strcmp(op, "count") == 0 ? count :
                                      strcmp(op, "mean") == 0 ? sum / count : sum));
        exprtk_map_set(&groups, group_key, acc);
    }
    return groups;
}

static exprtk_value_t fn_table_join(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 3 || args[0].type != EXPRTK_VAL_LIST || args[1].type != EXPRTK_VAL_LIST)
        return exprtk_val_num(0);
    char key_field[128];
    if (!table_arg_key(args[2], key_field, sizeof(key_field))) return exprtk_val_num(0);
    exprtk_value_t out = exprtk_val_list_empty();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t left = args[0].data.list.items[i];
        if (left.type != EXPRTK_VAL_MAP || !exprtk_map_has(&left, key_field)) continue;
        char lk[128];
        if (!table_key_from_value(exprtk_map_get(&left, key_field), lk, sizeof(lk))) continue;
        for (size_t j = 0; j < args[1].data.list.count; ++j) {
            exprtk_value_t right = args[1].data.list.items[j];
            if (right.type != EXPRTK_VAL_MAP || !exprtk_map_has(&right, key_field)) continue;
            char rk[128];
            if (!table_key_from_value(exprtk_map_get(&right, key_field), rk, sizeof(rk))) continue;
            if (strcmp(lk, rk) != 0) continue;
            exprtk_value_t row = table_clone_map(left);
            exprtk_map_iter_t it = exprtk_map_iter_begin(&right);
            const char *key = NULL;
            exprtk_value_t value;
            while (exprtk_map_iter_next(&it, &key, &value)) {
                if (strcmp(key, key_field) != 0)
                    exprtk_map_set(&row, key, value);
            }
            exprtk_list_push(&out, row);
        }
    }
    return out;
}

/* =========================================================================
 * 8. Module Definition
 * ========================================================================= */

static const exprtk_func_entry_t core_entries[] = {
    { "assert",    fn_assert },
    { "await",     fn_await },
    { "drop",      fn_drop },
    { "filter",    fn_filter },
    { "is_class",  fn_is_class },
    { "is_function", fn_is_function },
    { "is_instance", fn_is_instance },
    { "is_list",   fn_is_list },
    { "is_map",    fn_is_map },
    { "is_null",   fn_is_null },
    { "is_number", fn_is_number },
    { "is_string", fn_is_string },
    { "is_vector", fn_is_vector },
    { "lag",       fn_lag },
    { "list",      fn_list },
    { "map",       fn_map },
    { "print",     fn_print },
    { "reduce",    fn_reduce },
    { "range",     fn_range },
    { "table.filter", fn_table_filter },
    { "table.groupby", fn_table_groupby },
    { "table.join", fn_table_join },
    { "table.select", fn_table_select },
    { "take",      fn_take },
    { "typeof",    fn_typeof },
};

static const exprtk_module_t core_module = {
    "core", core_entries, sizeof(core_entries) / sizeof(core_entries[0])
};

const exprtk_module_t *exprtk_module_core(void) { return &core_module; }
