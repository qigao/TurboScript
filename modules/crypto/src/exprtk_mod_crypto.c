#include "crypto.h"
#include "monocypher.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static int crypto_digest_size_arg(size_t argc, exprtk_value_t *args, size_t index,
                                  size_t default_size, size_t *out_size) {
    if (argc <= index) {
        *out_size = default_size;
        return 1;
    }
    if (args[index].type == EXPRTK_VAL_INTEGER) {
        int64_t value = args[index].data.integer;
        if (value < 1 || value > 64) return 0;
        *out_size = (size_t)value;
        return 1;
    }
    if (args[index].type == EXPRTK_VAL_NUMBER) {
        double value = args[index].data.number;
        if (!isfinite(value)) return 0;
        if (value < 1.0 || value > 64.0) return 0;
        size_t size = (size_t)value;
        if ((double)size != value) return 0;
        *out_size = size;
        return 1;
    }
    return 0;
}

static exprtk_value_t crypto_hex_bytes(const uint8_t *bytes, size_t len, mem_pool_t *arena) {
    static const char hex[] = "0123456789abcdef";
    char *buf = (char *)mem_alloc(arena, len * 2 + 1);
    if (!buf) return exprtk_val_num(0);

    size_t pos = 0;
    for (size_t i = 0; i < len; ++i) {
        buf[pos++] = hex[bytes[i] >> 4];
        buf[pos++] = hex[bytes[i] & 0x0F];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_blake2b(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
    (void)env;
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    size_t digest_size = 64;
    if (!crypto_digest_size_arg(argc, args, 1, 64, &digest_size))
        return exprtk_val_num(0);

    tstr_v data = args[0].data.string;
    uint8_t digest[64];
    crypto_blake2b(digest, digest_size, (const uint8_t *)data.data, data.len);
    return crypto_hex_bytes(digest, digest_size, arena);
}

static exprtk_value_t fn_blake2b_keyed(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) ||
        args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    size_t digest_size = 64;
    if (!crypto_digest_size_arg(argc, args, 2, 64, &digest_size))
        return exprtk_val_num(0);

    tstr_v data = args[0].data.string;
    tstr_v key = args[1].data.string;
    if (key.len > 64)
        return exprtk_val_num(0);

    uint8_t digest[64];
    crypto_blake2b_keyed(digest, digest_size,
                         (const uint8_t *)key.data, key.len,
                         (const uint8_t *)data.data, data.len);
    return crypto_hex_bytes(digest, digest_size, arena);
}

static exprtk_value_t crypto_verify_fixed(size_t argc, exprtk_value_t *args, size_t len,
                                          int (*verify)(const uint8_t *, const uint8_t *)) {
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    tstr_v a = args[0].data.string;
    tstr_v b = args[1].data.string;
    if (a.len != len || b.len != len)
        return exprtk_val_num(0);

    return exprtk_val_num(verify((const uint8_t *)a.data, (const uint8_t *)b.data) == 0 ? 1.0 : 0.0);
}

static exprtk_value_t fn_verify16(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
    (void)env;
    (void)arena;
    return crypto_verify_fixed(argc, args, 16, crypto_verify16);
}

static exprtk_value_t fn_verify32(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
    (void)env;
    (void)arena;
    return crypto_verify_fixed(argc, args, 32, crypto_verify32);
}

static exprtk_value_t fn_verify64(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
    (void)env;
    (void)arena;
    return crypto_verify_fixed(argc, args, 64, crypto_verify64);
}

static const exprtk_func_entry_t crypto_entries[] = {
    {"blake2b", fn_blake2b},
    {"blake2b_keyed", fn_blake2b_keyed},
    {"verify16", fn_verify16},
    {"verify32", fn_verify32},
    {"verify64", fn_verify64},
};

static const exprtk_module_t crypto_module = {
    .module_name = "crypto",
    .entries = crypto_entries,
    .count = sizeof(crypto_entries) / sizeof(crypto_entries[0]),
};

const exprtk_module_t *exprtk_module_crypto(void) { return &crypto_module; }
