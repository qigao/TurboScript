#include "crypto.h"
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

spec("crypto_module") {
    describe("blake2b") {
        it("should hash strings with default digest size") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            const exprtk_module_t *mod = exprtk_module_crypto();
            exprtk_builtin_fn fn = find_function(mod, "blake2b");
            check_not_null(fn);

            exprtk_value_t args[1] = {str_arg("abc")};
            exprtk_value_t result = fn(1, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_STRING);
            check_str_eq(result.data.string.data,
                         "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");

            mem_destroy(&arena);
        }

        it("should support explicit digest size") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "blake2b");
            exprtk_value_t args[2] = {str_arg("abc"), exprtk_val_num(32)};
            exprtk_value_t result = fn(2, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_STRING);
            check_str_eq(result.data.string.data,
                         "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");

            mem_destroy(&arena);
        }

        it("should support keyed hashes") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "blake2b_keyed");
            exprtk_value_t args[3] = {str_arg("abc"), str_arg("key"), exprtk_val_num(4)};
            exprtk_value_t result = fn(3, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_STRING);
            check_str_eq(result.data.string.data, "34d401b4");

            mem_destroy(&arena);
        }

        it("should reject keys longer than BLAKE2b allows") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "blake2b_keyed");
            exprtk_value_t args[2] = {
                str_arg("abc"),
                str_arg("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
            };
            exprtk_value_t result = fn(2, args, NULL, &arena);

            check_float_eq(result.data.number, 0.0, 0.001);

            mem_destroy(&arena);
        }
    }

    describe("verify") {
        it("should compare fixed length strings") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "verify16");
            exprtk_value_t ok_args[2] = {str_arg("abcdefghijklmnop"),
                                         str_arg("abcdefghijklmnop")};
            exprtk_value_t bad_args[2] = {str_arg("abcdefghijklmnop"),
                                          str_arg("abcdefghijklmnoq")};

            exprtk_value_t ok = fn(2, ok_args, NULL, &arena);
            exprtk_value_t bad = fn(2, bad_args, NULL, &arena);

            check_float_eq(ok.data.number, 1.0, 0.001);
            check_float_eq(bad.data.number, 0.0, 0.001);

            mem_destroy(&arena);
        }

        it("should reject wrong lengths") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "verify32");
            exprtk_value_t args[2] = {str_arg("short"), str_arg("short")};
            exprtk_value_t result = fn(2, args, NULL, &arena);

            check_float_eq(result.data.number, 0.0, 0.001);

            mem_destroy(&arena);
        }
    }
}
