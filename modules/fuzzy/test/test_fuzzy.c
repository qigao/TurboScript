#include "fuzzy.h"
#include "tinytest.h"

#include <string.h>

static exprtk_builtin_fn find_function(const exprtk_module_t *mod, const char *name) {
    for (size_t i = 0; i < mod->count; ++i) {
        if (strcmp(mod->entries[i].name, name) == 0)
            return mod->entries[i].fn;
    }
    return NULL;
}

static exprtk_value_t str_arg(const char *s) {
    return exprtk_val_str(tstr_v_from_buf((char *)s, strlen(s)));
}

static exprtk_value_t map_get(exprtk_value_t *map, const char *key) {
    return exprtk_map_get(map, key);
}

spec("fuzzy_module") {
    describe("regex") {
        it("should return byte and utf8 offsets for utf8 text") {
            mem_pool_t arena;
            exprtk_builtin_fn fn;
            exprtk_value_t args[2];
            exprtk_value_t result;
            exprtk_value_t matched;
            exprtk_value_t start;
            exprtk_value_t utf8_start;
            exprtk_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(exprtk_module_fuzzy(), "regex");
            check_not_null(fn);

            args[0] = str_arg("caf.");
            args[1] = str_arg("xx caf\xC3\xA9 yy");
            result = fn(2, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_MAP);
            matched = map_get(&result, "matched");
            start = map_get(&result, "start");
            utf8_start = map_get(&result, "utf8_start");
            text = map_get(&result, "text");
            check_int_eq(matched.type, EXPRTK_VAL_NUMBER);
            check_float_eq(matched.data.number, 1.0, 0.001);
            check_int_eq(start.type, EXPRTK_VAL_INTEGER);
            check_int_eq((int)start.data.integer, 3);
            check_int_eq(utf8_start.type, EXPRTK_VAL_INTEGER);
            check_int_eq((int)utf8_start.data.integer, 3);
            check_int_eq(text.type, EXPRTK_VAL_STRING);
            check_str_eq(text.data.string.data, "caf\xC3\xA9");

            mem_destroy(&arena);
        }
    }

    describe("search") {
        it("should match utf8 text with substitution cost") {
            mem_pool_t arena;
            exprtk_builtin_fn fn;
            exprtk_value_t args[3];
            exprtk_value_t result;
            exprtk_value_t matched;
            exprtk_value_t cost;
            exprtk_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(exprtk_module_fuzzy(), "search");
            check_not_null(fn);

            args[0] = str_arg("cafe");
            args[1] = str_arg("xx caf\xC3\xA9 yy");
            args[2] = exprtk_val_int(1);
            result = fn(3, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_MAP);
            matched = map_get(&result, "matched");
            cost = map_get(&result, "cost");
            text = map_get(&result, "text");
            check_float_eq(matched.data.number, 1.0, 0.001);
            check_int_eq(cost.type, EXPRTK_VAL_INTEGER);
            check_int_eq((int)cost.data.integer, 1);
            check_int_eq(text.type, EXPRTK_VAL_STRING);
            check_str_eq(text.data.string.data, "caf\xC3\xA9");

            mem_destroy(&arena);
        }

        it("should treat metacharacters literally by default") {
            mem_pool_t arena;
            exprtk_builtin_fn fn;
            exprtk_value_t args[3];
            exprtk_value_t result;
            exprtk_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(exprtk_module_fuzzy(), "search");
            check_not_null(fn);

            args[0] = str_arg("a.c");
            args[1] = str_arg("xx a.c yy");
            args[2] = exprtk_val_int(0);
            result = fn(3, args, NULL, &arena);

            text = map_get(&result, "text");
            check_int_eq(text.type, EXPRTK_VAL_STRING);
            check_str_eq(text.data.string.data, "a.c");

            mem_destroy(&arena);
        }
    }

    describe("fuzzy_regex_search") {
        it("should allow approximate regex matching") {
            mem_pool_t arena;
            exprtk_builtin_fn fn;
            exprtk_value_t args[3];
            exprtk_value_t result;
            exprtk_value_t matched;

            mem_init(&arena, 4096);
            fn = find_function(exprtk_module_fuzzy(), "fuzzy_regex_search");
            check_not_null(fn);

            args[0] = str_arg("br.ve");
            args[1] = str_arg("hello brave world");
            args[2] = exprtk_val_int(0);
            result = fn(3, args, NULL, &arena);

            matched = map_get(&result, "matched");
            check_float_eq(matched.data.number, 1.0, 0.001);

            mem_destroy(&arena);
        }
    }

    describe("levenshtein") {
        it("should return utf8 approximate matches") {
            mem_pool_t arena;
            exprtk_builtin_fn fn;
            exprtk_value_t args[3];
            exprtk_value_t result;
            int found_full = 0;

            mem_init(&arena, 4096);
            fn = find_function(exprtk_module_fuzzy(), "levenshtein");
            check_not_null(fn);

            args[0] = str_arg("caf\xC3\xA9");
            args[1] = str_arg("xx cafe yy");
            args[2] = exprtk_val_int(1);
            result = fn(3, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_LIST);
            check(result.data.list.count > 0);
            for (size_t i = 0; i < result.data.list.count; ++i) {
                exprtk_value_t item = result.data.list.items[i];
                exprtk_value_t distance = map_get(&item, "distance");
                exprtk_value_t text = map_get(&item, "text");
                if (distance.type == EXPRTK_VAL_INTEGER && distance.data.integer == 1 &&
                    text.type == EXPRTK_VAL_STRING && strcmp(text.data.string.data, "cafe") == 0) {
                    found_full = 1;
                    break;
                }
            }
            check(found_full);

            mem_destroy(&arena);
        }
    }

    describe("ac") {
        it("should return multi-pattern utf8 matches") {
            mem_pool_t arena;
            exprtk_builtin_fn fn;
            exprtk_value_t patterns;
            exprtk_value_t args[2];
            exprtk_value_t result;
            exprtk_value_t item;
            exprtk_value_t pattern_id;
            exprtk_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(exprtk_module_fuzzy(), "ac");
            check_not_null(fn);

            patterns = exprtk_val_list_empty();
            exprtk_list_push(&patterns, str_arg("caf\xC3\xA9"));
            exprtk_list_push(&patterns, str_arg("tea"));
            args[0] = patterns;
            args[1] = str_arg("xx caf\xC3\xA9 tea");
            result = fn(2, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_LIST);
            check(result.data.list.count >= 2);
            item = result.data.list.items[0];
            pattern_id = map_get(&item, "pattern_id");
            text = map_get(&item, "text");
            check_int_eq(pattern_id.type, EXPRTK_VAL_INTEGER);
            check_int_eq((int)pattern_id.data.integer, 0);
            check_int_eq(text.type, EXPRTK_VAL_STRING);
            check_str_eq(text.data.string.data, "caf\xC3\xA9");

            mem_destroy(&arena);
        }
    }
}
