#include "hash.h"
#include "tinytest.h"
#include "xxhash.h"

#include <stdint.h>
#include <string.h>

static exprtk_builtin_fn find_function(const exprtk_module_t *mod, const char *name) {
    for (size_t i = 0; i < mod->count; ++i) {
        if (strcmp(mod->entries[i].name, name) == 0)
            return mod->entries[i].fn;
    }
    return NULL;
}

static exprtk_value_t str_arg(const char *s) {
    return exprtk_val_str(vstr_from_buf((char *)s, strlen(s)));
}

static void hex_u64(uint64_t value, char out[17]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[15 - i] = hex[(value >> (i * 4)) & 0x0F];
    }
    out[16] = '\0';
}

spec("hash_module") {
    describe("xxhash") {
        it("should return xxh32 as a number") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_hash(), "xxh32");
            check_not_null(fn);

            exprtk_value_t args[1] = {str_arg("abc")};
            exprtk_value_t result = fn(1, args, NULL, &arena);

            check((result.type) == (EXPRTK_VAL_NUMBER));
            check(fabs((double)(result.data.number) - (double)((double)XXH32("abc", 3, 0))) <= (double)(0.001));

            mem_destroy(&arena);
        }

        it("should return xxh64 as fixed width hex") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_hash(), "xxh64_hex");
            exprtk_value_t args[2] = {str_arg("abc"), exprtk_val_num(7)};
            exprtk_value_t result = fn(2, args, NULL, &arena);

            char expected[17];
            hex_u64((uint64_t)XXH64("abc", 3, 7), expected);

            check((result.type) == (EXPRTK_VAL_STRING));
            check(strcmp((result.data.string.data), (expected)) == 0);

            mem_destroy(&arena);
        }

        it("should return xxh3 64-bit as fixed width hex") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_hash(), "xxh3_64_hex");
            exprtk_value_t args[1] = {str_arg("abc")};
            exprtk_value_t result = fn(1, args, NULL, &arena);

            char expected[17];
            hex_u64((uint64_t)XXH3_64bits("abc", 3), expected);

            check((result.type) == (EXPRTK_VAL_STRING));
            check(strcmp((result.data.string.data), (expected)) == 0);

            mem_destroy(&arena);
        }

        it("should reject invalid seed types") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_hash(), "xxh32");
            exprtk_value_t args[2] = {str_arg("abc"), str_arg("bad")};
            exprtk_value_t result = fn(2, args, NULL, &arena);

            check(fabs((double)(result.data.number) - (double)(0.0)) <= (double)(0.001));

            mem_destroy(&arena);
        }
    }
}
