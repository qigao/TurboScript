#include "hash.h"
#include "xxhash.h"

#include <math.h>
#include <stdint.h>

static int hash_u64_arg(const exprtk_value_t *arg, uint64_t *out) {
    if (arg->type == EXPRTK_VAL_INTEGER) {
        if (arg->data.integer < 0) return 0;
        *out = (uint64_t)arg->data.integer;
        return 1;
    }
    if (arg->type == EXPRTK_VAL_NUMBER) {
        if (!isfinite(arg->data.number)) return 0;
        if (arg->data.number < 0.0) return 0;
        *out = (uint64_t)arg->data.number;
        return 1;
    }
    return 0;
}

static exprtk_value_t hash_hex_u64(uint64_t value, mem_pool_t *arena) {
    static const char hex[] = "0123456789abcdef";
    char *buf = (char *)mem_alloc(arena, 17);
    if (!buf) return exprtk_val_num(0);

    for (int i = 15; i >= 0; --i) {
        buf[15 - i] = hex[(value >> (i * 4)) & 0x0F];
    }
    buf[16] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, 16));
}

static exprtk_value_t fn_xxh32(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
    (void)env;
    (void)arena;
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    uint64_t seed64 = 0;
    if (argc == 2 && !hash_u64_arg(&args[1], &seed64))
        return exprtk_val_num(0);

    tstr_v data = args[0].data.string;
    XXH32_hash_t hash = XXH32(data.data, data.len, (XXH32_hash_t)seed64);
    return exprtk_val_num((double)hash);
}

static exprtk_value_t fn_xxh64_hex(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)env;
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    uint64_t seed = 0;
    if (argc == 2 && !hash_u64_arg(&args[1], &seed))
        return exprtk_val_num(0);

    tstr_v data = args[0].data.string;
    return hash_hex_u64((uint64_t)XXH64(data.data, data.len, (XXH64_hash_t)seed), arena);
}

static exprtk_value_t fn_xxh3_64_hex(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)env;
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    tstr_v data = args[0].data.string;
    XXH64_hash_t hash;
    if (argc == 2) {
        uint64_t seed = 0;
        if (!hash_u64_arg(&args[1], &seed))
            return exprtk_val_num(0);
        hash = XXH3_64bits_withSeed(data.data, data.len, (XXH64_hash_t)seed);
    } else {
        hash = XXH3_64bits(data.data, data.len);
    }
    return hash_hex_u64((uint64_t)hash, arena);
}

static const exprtk_func_entry_t hash_entries[] = {
    {"xxh32", fn_xxh32},
    {"xxh64_hex", fn_xxh64_hex},
    {"xxh3_64_hex", fn_xxh3_64_hex},
};

static const exprtk_module_t hash_module = {
    .module_name = "hash",
    .entries = hash_entries,
    .count = sizeof(hash_entries) / sizeof(hash_entries[0]),
};

const exprtk_module_t *exprtk_module_hash(void) { return &hash_module; }
