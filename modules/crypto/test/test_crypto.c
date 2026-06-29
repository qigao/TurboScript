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

static exprtk_value_t bytes_arg(const void *data, size_t len) {
    return exprtk_val_bytes(tstr_v_from_buf((char *)data, len));
}

spec("crypto_module") {
    describe("sha256") {
        it("should hash strings with SHA-256") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "sha256");
            check_not_null(fn);

            exprtk_value_t args[1] = {str_arg("abc")};
            exprtk_value_t result = fn(1, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_STRING);
            check_str_eq(result.data.string.data,
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

            mem_destroy(&arena);
        }

        it("should return raw SHA-256 bytes") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "sha256_bytes");
            check_not_null(fn);

            exprtk_value_t args[1] = {str_arg("abc")};
            exprtk_value_t result = fn(1, args, NULL, &arena);

            check_int_eq(result.type, EXPRTK_VAL_BYTES);
            check_size_eq(result.data.bytes.len, 32);
            check_int_eq((unsigned char)result.data.bytes.data[0], 0xba);

            mem_destroy(&arena);
        }
    }

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

    describe("aes") {
        it("should encrypt and decrypt AES-CTR bytes") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn enc = find_function(exprtk_module_crypto(), "aes_encrypt");
            exprtk_builtin_fn dec = find_function(exprtk_module_crypto(), "aes_decrypt");
            check_not_null(enc);
            check_not_null(dec);

            const unsigned char key[16] = {
                0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
            };
            const unsigned char iv[16] = {
                0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
            };
            const unsigned char plain[16] = {
                0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a
            };

            exprtk_value_t enc_args[3] = {
                bytes_arg(plain, sizeof(plain)),
                bytes_arg(key, sizeof(key)),
                bytes_arg(iv, sizeof(iv))
            };
            exprtk_value_t cipher = enc(3, enc_args, NULL, &arena);
            check_int_eq(cipher.type, EXPRTK_VAL_BYTES);
            check_size_eq(cipher.data.bytes.len, sizeof(plain));

            exprtk_value_t dec_args[3] = {
                cipher,
                bytes_arg(key, sizeof(key)),
                bytes_arg(iv, sizeof(iv))
            };
            exprtk_value_t roundtrip = dec(3, dec_args, NULL, &arena);
            check_int_eq(roundtrip.type, EXPRTK_VAL_BYTES);
            check_size_eq(roundtrip.data.bytes.len, sizeof(plain));
            check(memcmp(roundtrip.data.bytes.data, plain, sizeof(plain)) == 0);

            mem_destroy(&arena);
        }
    }

    describe("aead") {
        it("should lock and unlock with Monocypher AEAD") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn lock = find_function(exprtk_module_crypto(), "aead_lock");
            exprtk_builtin_fn unlock = find_function(exprtk_module_crypto(), "aead_unlock");
            check_not_null(lock);
            check_not_null(unlock);

            unsigned char key[32];
            unsigned char nonce[24];
            memset(key, 0x11, sizeof(key));
            memset(nonce, 0x22, sizeof(nonce));
            const char plain[] = "secret payload";
            const char ad[] = "header";

            exprtk_value_t lock_args[4] = {
                bytes_arg(plain, strlen(plain)),
                bytes_arg(key, sizeof(key)),
                bytes_arg(nonce, sizeof(nonce)),
                bytes_arg(ad, strlen(ad))
            };
            exprtk_value_t locked = lock(4, lock_args, NULL, &arena);
            check_int_eq(locked.type, EXPRTK_VAL_OBJECT);

            exprtk_value_t cipher = exprtk_map_get(&locked, "cipher");
            exprtk_value_t mac = exprtk_map_get(&locked, "mac");
            check_int_eq(cipher.type, EXPRTK_VAL_BYTES);
            check_int_eq(mac.type, EXPRTK_VAL_BYTES);
            check_size_eq(mac.data.bytes.len, 16);

            exprtk_value_t unlock_args[5] = {
                cipher,
                mac,
                bytes_arg(key, sizeof(key)),
                bytes_arg(nonce, sizeof(nonce)),
                bytes_arg(ad, strlen(ad))
            };
            exprtk_value_t opened = unlock(5, unlock_args, NULL, &arena);
            check_int_eq(opened.type, EXPRTK_VAL_BYTES);
            check_size_eq(opened.data.bytes.len, strlen(plain));
            check(memcmp(opened.data.bytes.data, plain, strlen(plain)) == 0);

            mem_destroy(&arena);
        }

        it("should reject invalid AEAD MAC") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn lock = find_function(exprtk_module_crypto(), "aead_lock");
            exprtk_builtin_fn unlock = find_function(exprtk_module_crypto(), "aead_unlock");
            unsigned char key[32];
            unsigned char nonce[24];
            memset(key, 0x33, sizeof(key));
            memset(nonce, 0x44, sizeof(nonce));
            const char plain[] = "secret";

            exprtk_value_t lock_args[4] = {
                bytes_arg(plain, strlen(plain)),
                bytes_arg(key, sizeof(key)),
                bytes_arg(nonce, sizeof(nonce)),
                bytes_arg("", 0)
            };
            exprtk_value_t locked = lock(4, lock_args, NULL, &arena);
            exprtk_value_t cipher = exprtk_map_get(&locked, "cipher");
            exprtk_value_t mac = exprtk_map_get(&locked, "mac");
            unsigned char bad_mac[16];
            memcpy(bad_mac, mac.data.bytes.data, sizeof(bad_mac));
            bad_mac[0] ^= 0x01;

            exprtk_value_t unlock_args[5] = {
                cipher,
                bytes_arg(bad_mac, sizeof(bad_mac)),
                bytes_arg(key, sizeof(key)),
                bytes_arg(nonce, sizeof(nonce)),
                bytes_arg("", 0)
            };
            exprtk_value_t opened = unlock(5, unlock_args, NULL, &arena);
            check_int_eq(opened.type, EXPRTK_VAL_NULL);

            mem_destroy(&arena);
        }
    }

    describe("argon2") {
        it("should derive deterministic Argon2 keys") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn fn = find_function(exprtk_module_crypto(), "argon2");
            check_not_null(fn);

            exprtk_value_t args[6] = {
                str_arg("password"),
                str_arg("0123456789abcdef"),
                exprtk_val_num(32),
                exprtk_val_num(8),
                exprtk_val_num(1),
                exprtk_val_num(2)
            };
            exprtk_value_t first = fn(6, args, NULL, &arena);
            exprtk_value_t second = fn(6, args, NULL, &arena);

            check_int_eq(first.type, EXPRTK_VAL_BYTES);
            check_int_eq(second.type, EXPRTK_VAL_BYTES);
            check_size_eq(first.data.bytes.len, 32);
            check(memcmp(first.data.bytes.data, second.data.bytes.data, 32) == 0);

            mem_destroy(&arena);
        }
    }

    describe("x25519") {
        it("should derive matching shared secrets") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn public_key = find_function(exprtk_module_crypto(), "x25519_public_key");
            exprtk_builtin_fn shared = find_function(exprtk_module_crypto(), "x25519");
            check_not_null(public_key);
            check_not_null(shared);

            unsigned char alice_secret[32];
            unsigned char bob_secret[32];
            memset(alice_secret, 0x11, sizeof(alice_secret));
            memset(bob_secret, 0x22, sizeof(bob_secret));

            exprtk_value_t alice_pk_args[1] = {bytes_arg(alice_secret, sizeof(alice_secret))};
            exprtk_value_t bob_pk_args[1] = {bytes_arg(bob_secret, sizeof(bob_secret))};
            exprtk_value_t alice_public = public_key(1, alice_pk_args, NULL, &arena);
            exprtk_value_t bob_public = public_key(1, bob_pk_args, NULL, &arena);

            exprtk_value_t alice_shared_args[2] = {
                bytes_arg(alice_secret, sizeof(alice_secret)),
                bob_public
            };
            exprtk_value_t bob_shared_args[2] = {
                bytes_arg(bob_secret, sizeof(bob_secret)),
                alice_public
            };
            exprtk_value_t alice_shared = shared(2, alice_shared_args, NULL, &arena);
            exprtk_value_t bob_shared = shared(2, bob_shared_args, NULL, &arena);

            check_int_eq(alice_shared.type, EXPRTK_VAL_BYTES);
            check_size_eq(alice_shared.data.bytes.len, 32);
            check(memcmp(alice_shared.data.bytes.data, bob_shared.data.bytes.data, 32) == 0);

            mem_destroy(&arena);
        }
    }

    describe("eddsa") {
        it("should sign and verify messages") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn key_pair = find_function(exprtk_module_crypto(), "eddsa_key_pair");
            exprtk_builtin_fn sign = find_function(exprtk_module_crypto(), "eddsa_sign");
            exprtk_builtin_fn check_sig = find_function(exprtk_module_crypto(), "eddsa_check");
            check_not_null(key_pair);
            check_not_null(sign);
            check_not_null(check_sig);

            unsigned char seed[32];
            memset(seed, 0x7a, sizeof(seed));
            exprtk_value_t kp_args[1] = {bytes_arg(seed, sizeof(seed))};
            exprtk_value_t kp = key_pair(1, kp_args, NULL, &arena);
            check_int_eq(kp.type, EXPRTK_VAL_OBJECT);

            exprtk_value_t secret_key = exprtk_map_get(&kp, "secret_key");
            exprtk_value_t public_key = exprtk_map_get(&kp, "public_key");
            check_int_eq(secret_key.type, EXPRTK_VAL_BYTES);
            check_int_eq(public_key.type, EXPRTK_VAL_BYTES);
            check_size_eq(secret_key.data.bytes.len, 64);
            check_size_eq(public_key.data.bytes.len, 32);

            exprtk_value_t sign_args[2] = {secret_key, str_arg("message")};
            exprtk_value_t signature = sign(2, sign_args, NULL, &arena);
            check_int_eq(signature.type, EXPRTK_VAL_BYTES);
            check_size_eq(signature.data.bytes.len, 64);

            exprtk_value_t ok_args[3] = {signature, public_key, str_arg("message")};
            exprtk_value_t bad_args[3] = {signature, public_key, str_arg("tampered")};
            exprtk_value_t ok = check_sig(3, ok_args, NULL, &arena);
            exprtk_value_t bad = check_sig(3, bad_args, NULL, &arena);
            check_float_eq(ok.data.number, 1.0, 0.001);
            check_float_eq(bad.data.number, 0.0, 0.001);

            mem_destroy(&arena);
        }
    }

    describe("chacha20_poly1305") {
        it("should encrypt and decrypt with XChaCha20 and authenticate with Poly1305") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn chacha = find_function(exprtk_module_crypto(), "chacha20_x");
            exprtk_builtin_fn poly = find_function(exprtk_module_crypto(), "poly1305");
            check_not_null(chacha);
            check_not_null(poly);

            unsigned char key[32];
            unsigned char nonce[24];
            memset(key, 0x55, sizeof(key));
            memset(nonce, 0x66, sizeof(nonce));
            const char plain[] = "stream payload";

            exprtk_value_t enc_args[4] = {
                bytes_arg(plain, strlen(plain)),
                bytes_arg(key, sizeof(key)),
                bytes_arg(nonce, sizeof(nonce)),
                exprtk_val_num(0)
            };
            exprtk_value_t cipher = chacha(4, enc_args, NULL, &arena);
            check_int_eq(cipher.type, EXPRTK_VAL_BYTES);

            exprtk_value_t dec_args[4] = {
                cipher,
                bytes_arg(key, sizeof(key)),
                bytes_arg(nonce, sizeof(nonce)),
                exprtk_val_num(0)
            };
            exprtk_value_t opened = chacha(4, dec_args, NULL, &arena);
            check_int_eq(opened.type, EXPRTK_VAL_BYTES);
            check(memcmp(opened.data.bytes.data, plain, strlen(plain)) == 0);

            exprtk_value_t mac_args[2] = {cipher, bytes_arg(key, sizeof(key))};
            exprtk_value_t mac = poly(2, mac_args, NULL, &arena);
            check_int_eq(mac.type, EXPRTK_VAL_BYTES);
            check_size_eq(mac.data.bytes.len, 16);

            mem_destroy(&arena);
        }
    }

    describe("elligator") {
        it("should create representable X25519 keys") {
            mem_pool_t arena;
            mem_init(&arena, 4096);

            exprtk_builtin_fn key_pair = find_function(exprtk_module_crypto(), "elligator_key_pair");
            exprtk_builtin_fn map = find_function(exprtk_module_crypto(), "elligator_map");
            exprtk_builtin_fn dirty = find_function(exprtk_module_crypto(), "x25519_dirty_fast");
            exprtk_builtin_fn rev = find_function(exprtk_module_crypto(), "elligator_rev");
            check_not_null(key_pair);
            check_not_null(map);
            check_not_null(dirty);
            check_not_null(rev);

            unsigned char seed[32];
            memset(seed, 0x42, sizeof(seed));
            exprtk_value_t kp_args[1] = {bytes_arg(seed, sizeof(seed))};
            exprtk_value_t kp = key_pair(1, kp_args, NULL, &arena);
            check_int_eq(kp.type, EXPRTK_VAL_OBJECT);

            exprtk_value_t hidden = exprtk_map_get(&kp, "hidden");
            exprtk_value_t secret_key = exprtk_map_get(&kp, "secret_key");
            check_int_eq(hidden.type, EXPRTK_VAL_BYTES);
            check_int_eq(secret_key.type, EXPRTK_VAL_BYTES);

            exprtk_value_t map_args[1] = {hidden};
            exprtk_value_t curve_from_hidden = map(1, map_args, NULL, &arena);
            exprtk_value_t dirty_args[1] = {secret_key};
            exprtk_value_t curve_from_secret = dirty(1, dirty_args, NULL, &arena);
            check_int_eq(curve_from_hidden.type, EXPRTK_VAL_BYTES);
            check_int_eq(curve_from_secret.type, EXPRTK_VAL_BYTES);
            check(memcmp(curve_from_hidden.data.bytes.data,
                         curve_from_secret.data.bytes.data, 32) == 0);

            exprtk_value_t rev_args[2] = {curve_from_hidden, exprtk_val_num(0)};
            exprtk_value_t recovered = rev(2, rev_args, NULL, &arena);
            check_int_eq(recovered.type, EXPRTK_VAL_BYTES);
            check_size_eq(recovered.data.bytes.len, 32);

            mem_destroy(&arena);
        }
    }
}
