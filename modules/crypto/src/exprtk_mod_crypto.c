#include "crypto.h"

#include "aes.h"
#include "turbo_crypto.h"

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CRYPTO_ARGON2_BLOCK_SIZE 1024u
#define CRYPTO_ARGON2_MAX_BLOCKS 262144u

static exprtk_value_t crypto_null(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static int crypto_value_bytes(exprtk_value_t value, tstr_v *out) {
    if (value.type == EXPRTK_VAL_BYTES) {
        *out = value.data.bytes;
        return 1;
    }
    if (value.type == EXPRTK_VAL_STRING) {
        *out = value.data.string;
        return 1;
    }
    return 0;
}

static int crypto_value_u32(exprtk_value_t value, uint32_t min_value, uint32_t max_value,
                            uint32_t *out) {
    double numeric;
    if (value.type == EXPRTK_VAL_INTEGER) {
        if (value.data.integer < 0) return 0;
        if ((uint64_t)value.data.integer > UINT32_MAX) return 0;
        numeric = (double)value.data.integer;
    } else if (value.type == EXPRTK_VAL_NUMBER) {
        if (!isfinite(value.data.number)) return 0;
        numeric = value.data.number;
    } else {
        return 0;
    }
    if (numeric < (double)min_value || numeric > (double)max_value) return 0;
    uint32_t value_u32 = (uint32_t)numeric;
    if ((double)value_u32 != numeric) return 0;
    *out = value_u32;
    return 1;
}

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
    if (!buf) return crypto_null();

    size_t pos = 0;
    for (size_t i = 0; i < len; ++i) {
        buf[pos++] = hex[bytes[i] >> 4];
        buf[pos++] = hex[bytes[i] & 0x0F];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t crypto_copy_bytes(const uint8_t *bytes, size_t len, mem_pool_t *arena) {
    char *buf = NULL;
    if (len > 0) {
        buf = (char *)mem_alloc(arena, len);
        if (!buf) return crypto_null();
        memcpy(buf, bytes, len);
    }
    return exprtk_val_bytes(tstr_v_from_buf(buf, len));
}

static exprtk_value_t crypto_bytes32_unary(size_t argc, exprtk_value_t *args, mem_pool_t *arena,
                                           int (*fn)(uint8_t[32], const uint8_t[32])) {
    tstr_v in;
    if (argc != 1 || !crypto_value_bytes(args[0], &in) || in.len != 32)
        return crypto_null();
    uint8_t out[32];
    if (fn(out, (const uint8_t *)in.data) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(out, sizeof(out), arena);
}

static exprtk_value_t crypto_bytes32_binary(size_t argc, exprtk_value_t *args, mem_pool_t *arena,
                                            int (*fn)(uint8_t[32], const uint8_t[32],
                                                      const uint8_t[32])) {
    tstr_v a;
    tstr_v b;
    if (argc != 2 ||
        !crypto_value_bytes(args[0], &a) ||
        !crypto_value_bytes(args[1], &b) ||
        a.len != 32 || b.len != 32)
        return crypto_null();
    uint8_t out[32];
    if (fn(out, (const uint8_t *)a.data, (const uint8_t *)b.data) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(out, sizeof(out), arena);
}

static exprtk_value_t fn_sha256(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
    (void)env;
    tstr_v data;
    if (argc != 1 || !crypto_value_bytes(args[0], &data))
        return crypto_null();

    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    if (turbo_crypto_sha256(data.data, data.len, digest) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_hex_bytes(digest, sizeof(digest), arena);
}

static exprtk_value_t fn_sha256_bytes(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
    (void)env;
    tstr_v data;
    if (argc != 1 || !crypto_value_bytes(args[0], &data))
        return crypto_null();

    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    if (turbo_crypto_sha256(data.data, data.len, digest) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(digest, sizeof(digest), arena);
}

static exprtk_value_t fn_blake2b(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
    (void)env;
    tstr_v data;
    if ((argc != 1 && argc != 2) || !crypto_value_bytes(args[0], &data))
        return crypto_null();

    size_t digest_size = 64;
    if (!crypto_digest_size_arg(argc, args, 1, 64, &digest_size))
        return crypto_null();

    uint8_t digest[64];
    if (turbo_crypto_blake2b(digest, digest_size, data.data, data.len) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_hex_bytes(digest, digest_size, arena);
}

static exprtk_value_t fn_blake2b_keyed(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    tstr_v data;
    tstr_v key;
    if ((argc != 2 && argc != 3) ||
        !crypto_value_bytes(args[0], &data) ||
        !crypto_value_bytes(args[1], &key))
        return crypto_null();

    size_t digest_size = 64;
    if (!crypto_digest_size_arg(argc, args, 2, 64, &digest_size))
        return crypto_null();
    if (key.len > 64)
        return crypto_null();

    uint8_t digest[64];
    if (turbo_crypto_blake2b_keyed(digest, digest_size, key.data, key.len,
                                   data.data, data.len) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_hex_bytes(digest, digest_size, arena);
}

static exprtk_value_t crypto_verify_fixed(size_t argc, exprtk_value_t *args, size_t len) {
    tstr_v a;
    tstr_v b;
    if (argc != 2 || !crypto_value_bytes(args[0], &a) || !crypto_value_bytes(args[1], &b))
        return exprtk_val_num(0);
    if (a.len != len || b.len != len)
        return exprtk_val_num(0);

    return exprtk_val_num(turbo_crypto_verify(a.data, b.data, len) == TURBO_CRYPTO_OK ? 1.0 : 0.0);
}

static exprtk_value_t fn_verify16(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
    (void)env;
    (void)arena;
    return crypto_verify_fixed(argc, args, 16);
}

static exprtk_value_t fn_verify32(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
    (void)env;
    (void)arena;
    return crypto_verify_fixed(argc, args, 32);
}

static exprtk_value_t fn_verify64(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
    (void)env;
    (void)arena;
    return crypto_verify_fixed(argc, args, 64);
}

static exprtk_value_t fn_aes_ctr_crypt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    tstr_v data;
    tstr_v key;
    tstr_v iv;
    if (argc != 3 ||
        !crypto_value_bytes(args[0], &data) ||
        !crypto_value_bytes(args[1], &key) ||
        !crypto_value_bytes(args[2], &iv))
        return crypto_null();
    if (key.len != AES_KEYLEN || iv.len != AES_BLOCKLEN)
        return crypto_null();

    char *buf = NULL;
    if (data.len > 0) {
        buf = (char *)mem_alloc(arena, data.len);
        if (!buf) return crypto_null();
        memcpy(buf, data.data, data.len);
    }

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, (const uint8_t *)key.data, (const uint8_t *)iv.data);
    AES_CTR_xcrypt_buffer(&ctx, (uint8_t *)buf, data.len);
    return exprtk_val_bytes(tstr_v_from_buf(buf, data.len));
}

static exprtk_value_t fn_aead_lock(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)env;
    tstr_v plain;
    tstr_v key;
    tstr_v nonce;
    tstr_v ad;
    if (argc != 4 ||
        !crypto_value_bytes(args[0], &plain) ||
        !crypto_value_bytes(args[1], &key) ||
        !crypto_value_bytes(args[2], &nonce) ||
        !crypto_value_bytes(args[3], &ad))
        return crypto_null();
    if (key.len != 32 || nonce.len != 24)
        return crypto_null();

    char *cipher = NULL;
    if (plain.len > 0) {
        cipher = (char *)mem_alloc(arena, plain.len);
        if (!cipher) return crypto_null();
    }
    uint8_t mac[TURBO_CRYPTO_AEAD_MAC_SIZE];
    if (turbo_crypto_aead_lock(cipher, mac,
                               (const uint8_t *)key.data,
                               (const uint8_t *)nonce.data,
                               ad.data, ad.len, plain.data, plain.len) != TURBO_CRYPTO_OK)
        return crypto_null();

    exprtk_value_t result = exprtk_val_object();
    exprtk_map_set(&result, "cipher", exprtk_val_bytes(tstr_v_from_buf(cipher, plain.len)));
    exprtk_map_set(&result, "mac", crypto_copy_bytes(mac, sizeof(mac), arena));
    return result;
}

static exprtk_value_t fn_aead_unlock(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)env;
    tstr_v cipher;
    tstr_v mac;
    tstr_v key;
    tstr_v nonce;
    tstr_v ad;
    if (argc != 5 ||
        !crypto_value_bytes(args[0], &cipher) ||
        !crypto_value_bytes(args[1], &mac) ||
        !crypto_value_bytes(args[2], &key) ||
        !crypto_value_bytes(args[3], &nonce) ||
        !crypto_value_bytes(args[4], &ad))
        return crypto_null();
    if (mac.len != 16 || key.len != 32 || nonce.len != 24)
        return crypto_null();

    char *plain = NULL;
    if (cipher.len > 0) {
        plain = (char *)mem_alloc(arena, cipher.len);
        if (!plain) return crypto_null();
    }

    if (turbo_crypto_aead_unlock(plain,
                                 (const uint8_t *)mac.data,
                                 (const uint8_t *)key.data,
                                 (const uint8_t *)nonce.data,
                                 ad.data, ad.len, cipher.data, cipher.len) != TURBO_CRYPTO_OK)
        return crypto_null();

    return exprtk_val_bytes(tstr_v_from_buf(plain, cipher.len));
}

static exprtk_value_t fn_argon2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
    (void)env;
    if (argc != 6 && argc != 8)
        return crypto_null();

    tstr_v pass;
    tstr_v salt;
    if (!crypto_value_bytes(args[0], &pass) || !crypto_value_bytes(args[1], &salt))
        return crypto_null();

    uint32_t hash_size;
    uint32_t nb_blocks;
    uint32_t nb_passes;
    uint32_t algorithm;
    if (!crypto_value_u32(args[2], 1, 1024, &hash_size) ||
        !crypto_value_u32(args[3], 8, CRYPTO_ARGON2_MAX_BLOCKS, &nb_blocks) ||
        !crypto_value_u32(args[4], 1, UINT32_MAX, &nb_passes) ||
        !crypto_value_u32(args[5], TURBO_CRYPTO_ARGON2_D, TURBO_CRYPTO_ARGON2_ID, &algorithm))
        return crypto_null();

    tstr_v key = {0};
    tstr_v ad = {0};
    if (argc == 8 &&
        (!crypto_value_bytes(args[6], &key) || !crypto_value_bytes(args[7], &ad)))
        return crypto_null();
    if (pass.len > UINT32_MAX || salt.len > UINT32_MAX ||
        key.len > UINT32_MAX || ad.len > UINT32_MAX)
        return crypto_null();

    size_t work_size = (size_t)nb_blocks * CRYPTO_ARGON2_BLOCK_SIZE;
    void *work_area = malloc(work_size);
    if (!work_area)
        return crypto_null();

    uint8_t *hash = (uint8_t *)malloc(hash_size);
    if (!hash) {
        turbo_crypto_wipe(work_area, work_size);
        free(work_area);
        return crypto_null();
    }

    turbo_crypto_argon2_config_t config = {
        .algorithm = algorithm,
        .block_count = nb_blocks,
        .pass_count = nb_passes,
        .lane_count = 1,
    };
    turbo_crypto_argon2_inputs_t inputs = {
        .password = (const uint8_t *)pass.data,
        .salt = (const uint8_t *)salt.data,
        .password_size = (uint32_t)pass.len,
        .salt_size = (uint32_t)salt.len,
    };
    turbo_crypto_argon2_extras_t extras = {
        .key = (const uint8_t *)key.data,
        .associated_data = (const uint8_t *)ad.data,
        .key_size = (uint32_t)key.len,
        .associated_data_size = (uint32_t)ad.len,
    };

    if (turbo_crypto_argon2(hash, hash_size, work_area, config, inputs, extras) !=
        TURBO_CRYPTO_OK) {
        turbo_crypto_wipe(hash, hash_size);
        turbo_crypto_wipe(work_area, work_size);
        free(hash);
        free(work_area);
        return crypto_null();
    }
    exprtk_value_t result = crypto_copy_bytes(hash, hash_size, arena);
    turbo_crypto_wipe(hash, hash_size);
    turbo_crypto_wipe(work_area, work_size);
    free(hash);
    free(work_area);
    return result;
}

static exprtk_value_t fn_x25519_public_key(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_x25519_public_key);
}

static exprtk_value_t fn_x25519(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_binary(argc, args, arena, turbo_crypto_x25519);
}

static exprtk_value_t fn_x25519_to_eddsa(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_x25519_to_eddsa);
}

static exprtk_value_t fn_x25519_inverse(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_binary(argc, args, arena, turbo_crypto_x25519_inverse);
}

static exprtk_value_t fn_x25519_dirty_small(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                            mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_x25519_dirty_small);
}

static exprtk_value_t fn_x25519_dirty_fast(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_x25519_dirty_fast);
}

static exprtk_value_t fn_eddsa_key_pair(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)env;
    tstr_v seed;
    if (argc != 1 || !crypto_value_bytes(args[0], &seed) || seed.len != 32)
        return crypto_null();
    uint8_t seed_copy[32];
    uint8_t secret_key[64];
    uint8_t public_key[32];
    memcpy(seed_copy, seed.data, sizeof(seed_copy));
    if (turbo_crypto_eddsa_key_pair(secret_key, public_key, seed_copy) != TURBO_CRYPTO_OK)
        return crypto_null();

    exprtk_value_t result = exprtk_val_object();
    exprtk_map_set(&result, "secret_key", crypto_copy_bytes(secret_key, sizeof(secret_key), arena));
    exprtk_map_set(&result, "public_key", crypto_copy_bytes(public_key, sizeof(public_key), arena));
    turbo_crypto_wipe(seed_copy, sizeof(seed_copy));
    turbo_crypto_wipe(secret_key, sizeof(secret_key));
    return result;
}

static exprtk_value_t fn_eddsa_sign(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
    (void)env;
    tstr_v secret_key;
    tstr_v message;
    if (argc != 2 ||
        !crypto_value_bytes(args[0], &secret_key) ||
        !crypto_value_bytes(args[1], &message) ||
        secret_key.len != 64)
        return crypto_null();
    uint8_t signature[64];
    if (turbo_crypto_eddsa_sign(signature, (const uint8_t *)secret_key.data,
                                message.data, message.len) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(signature, sizeof(signature), arena);
}

static exprtk_value_t fn_eddsa_check(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)env;
    (void)arena;
    tstr_v signature;
    tstr_v public_key;
    tstr_v message;
    if (argc != 3 ||
        !crypto_value_bytes(args[0], &signature) ||
        !crypto_value_bytes(args[1], &public_key) ||
        !crypto_value_bytes(args[2], &message) ||
        signature.len != 64 || public_key.len != 32)
        return exprtk_val_num(0);
    return exprtk_val_num(turbo_crypto_eddsa_check((const uint8_t *)signature.data,
                                                   (const uint8_t *)public_key.data,
                                                   message.data, message.len) == TURBO_CRYPTO_OK
                              ? 1.0
                              : 0.0);
}

static exprtk_value_t fn_eddsa_to_x25519(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_eddsa_to_x25519);
}

static exprtk_value_t fn_eddsa_trim_scalar(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_eddsa_trim_scalar);
}

static exprtk_value_t fn_eddsa_reduce(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
    (void)env;
    tstr_v expanded;
    if (argc != 1 || !crypto_value_bytes(args[0], &expanded) || expanded.len != 64)
        return crypto_null();
    uint8_t reduced[32];
    if (turbo_crypto_eddsa_reduce(reduced, (const uint8_t *)expanded.data) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(reduced, sizeof(reduced), arena);
}

static exprtk_value_t fn_eddsa_mul_add(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    tstr_v a;
    tstr_v b;
    tstr_v c;
    if (argc != 3 ||
        !crypto_value_bytes(args[0], &a) ||
        !crypto_value_bytes(args[1], &b) ||
        !crypto_value_bytes(args[2], &c) ||
        a.len != 32 || b.len != 32 || c.len != 32)
        return crypto_null();
    uint8_t out[32];
    if (turbo_crypto_eddsa_mul_add(out, (const uint8_t *)a.data,
                                   (const uint8_t *)b.data,
                                   (const uint8_t *)c.data) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(out, sizeof(out), arena);
}

static exprtk_value_t fn_eddsa_scalarbase(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_eddsa_scalarbase);
}

static exprtk_value_t fn_chacha20_h(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
    (void)env;
    tstr_v key;
    tstr_v in;
    if (argc != 2 ||
        !crypto_value_bytes(args[0], &key) ||
        !crypto_value_bytes(args[1], &in) ||
        key.len != 32 || in.len != 16)
        return crypto_null();
    uint8_t out[32];
    if (turbo_crypto_chacha20_h(out, (const uint8_t *)key.data,
                                (const uint8_t *)in.data) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(out, sizeof(out), arena);
}

static exprtk_value_t crypto_chacha20_crypt(size_t argc, exprtk_value_t *args,
                                            mem_pool_t *arena, size_t nonce_len) {
    tstr_v data;
    tstr_v key;
    tstr_v nonce;
    uint32_t ctr32 = 0;
    if ((argc != 3 && argc != 4) ||
        !crypto_value_bytes(args[0], &data) ||
        !crypto_value_bytes(args[1], &key) ||
        !crypto_value_bytes(args[2], &nonce) ||
        key.len != 32 || nonce.len != nonce_len)
        return crypto_null();
    if (argc == 4 && !crypto_value_u32(args[3], 0, UINT32_MAX, &ctr32))
        return crypto_null();

    char *buf = NULL;
    if (data.len > 0) {
        buf = (char *)mem_alloc(arena, data.len);
        if (!buf) return crypto_null();
    }

    if (nonce_len == 8) {
        if (turbo_crypto_chacha20_djb(buf, data.data, data.len,
                                      (const uint8_t *)key.data,
                                      (const uint8_t *)nonce.data,
                                      (uint64_t)ctr32) != TURBO_CRYPTO_OK)
            return crypto_null();
    } else if (nonce_len == 12) {
        if (turbo_crypto_chacha20_ietf(buf, data.data, data.len,
                                       (const uint8_t *)key.data,
                                       (const uint8_t *)nonce.data,
                                       ctr32) != TURBO_CRYPTO_OK)
            return crypto_null();
    } else {
        if (turbo_crypto_chacha20_x(buf, data.data, data.len,
                                    (const uint8_t *)key.data,
                                    (const uint8_t *)nonce.data,
                                    (uint64_t)ctr32) != TURBO_CRYPTO_OK)
            return crypto_null();
    }
    return exprtk_val_bytes(tstr_v_from_buf(buf, data.len));
}

static exprtk_value_t fn_chacha20_djb(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
    (void)env;
    return crypto_chacha20_crypt(argc, args, arena, 8);
}

static exprtk_value_t fn_chacha20_ietf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    return crypto_chacha20_crypt(argc, args, arena, 12);
}

static exprtk_value_t fn_chacha20_x(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
    (void)env;
    return crypto_chacha20_crypt(argc, args, arena, 24);
}

static exprtk_value_t fn_poly1305(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
    (void)env;
    tstr_v message;
    tstr_v key;
    if (argc != 2 ||
        !crypto_value_bytes(args[0], &message) ||
        !crypto_value_bytes(args[1], &key) ||
        key.len != 32)
        return crypto_null();
    uint8_t mac[16];
    if (turbo_crypto_poly1305(mac, message.data, message.len,
                              (const uint8_t *)key.data) != TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(mac, sizeof(mac), arena);
}

static exprtk_value_t fn_elligator_map(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    return crypto_bytes32_unary(argc, args, arena, turbo_crypto_elligator_map);
}

static exprtk_value_t fn_elligator_rev(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    tstr_v curve;
    uint32_t tweak;
    if (argc != 2 ||
        !crypto_value_bytes(args[0], &curve) ||
        curve.len != 32 ||
        !crypto_value_u32(args[1], 0, 255, &tweak))
        return crypto_null();
    uint8_t hidden[32];
    if (turbo_crypto_elligator_rev(hidden, (const uint8_t *)curve.data, (uint8_t)tweak) !=
        TURBO_CRYPTO_OK)
        return crypto_null();
    return crypto_copy_bytes(hidden, sizeof(hidden), arena);
}

static exprtk_value_t fn_elligator_key_pair(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                            mem_pool_t *arena) {
    (void)env;
    tstr_v seed;
    if (argc != 1 || !crypto_value_bytes(args[0], &seed) || seed.len != 32)
        return crypto_null();
    uint8_t seed_copy[32];
    uint8_t hidden[32];
    uint8_t secret_key[32];
    memcpy(seed_copy, seed.data, sizeof(seed_copy));
    if (turbo_crypto_elligator_key_pair(hidden, secret_key, seed_copy) != TURBO_CRYPTO_OK)
        return crypto_null();

    exprtk_value_t result = exprtk_val_object();
    exprtk_map_set(&result, "hidden", crypto_copy_bytes(hidden, sizeof(hidden), arena));
    exprtk_map_set(&result, "secret_key", crypto_copy_bytes(secret_key, sizeof(secret_key), arena));
    turbo_crypto_wipe(seed_copy, sizeof(seed_copy));
    turbo_crypto_wipe(secret_key, sizeof(secret_key));
    return result;
}

static const exprtk_func_entry_t crypto_entries[] = {
    {"sha256", fn_sha256},
    {"sha256_bytes", fn_sha256_bytes},
    {"blake2b", fn_blake2b},
    {"blake2b_keyed", fn_blake2b_keyed},
    {"aes_encrypt", fn_aes_ctr_crypt},
    {"aes_decrypt", fn_aes_ctr_crypt},
    {"aes_ctr_encrypt", fn_aes_ctr_crypt},
    {"aes_ctr_decrypt", fn_aes_ctr_crypt},
    {"aead_lock", fn_aead_lock},
    {"aead_unlock", fn_aead_unlock},
    {"argon2", fn_argon2},
    {"x25519_public_key", fn_x25519_public_key},
    {"x25519", fn_x25519},
    {"x25519_to_eddsa", fn_x25519_to_eddsa},
    {"x25519_inverse", fn_x25519_inverse},
    {"x25519_dirty_small", fn_x25519_dirty_small},
    {"x25519_dirty_fast", fn_x25519_dirty_fast},
    {"eddsa_key_pair", fn_eddsa_key_pair},
    {"eddsa_sign", fn_eddsa_sign},
    {"eddsa_check", fn_eddsa_check},
    {"eddsa_to_x25519", fn_eddsa_to_x25519},
    {"eddsa_trim_scalar", fn_eddsa_trim_scalar},
    {"eddsa_reduce", fn_eddsa_reduce},
    {"eddsa_mul_add", fn_eddsa_mul_add},
    {"eddsa_scalarbase", fn_eddsa_scalarbase},
    {"chacha20_h", fn_chacha20_h},
    {"chacha20_djb", fn_chacha20_djb},
    {"chacha20_ietf", fn_chacha20_ietf},
    {"chacha20_x", fn_chacha20_x},
    {"poly1305", fn_poly1305},
    {"elligator_map", fn_elligator_map},
    {"elligator_rev", fn_elligator_rev},
    {"elligator_key_pair", fn_elligator_key_pair},
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
