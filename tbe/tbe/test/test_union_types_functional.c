/**
 * @file test_union_types_functional.c
 * @brief Functional BDD tests for union type code generation.
 *
 * Tests verify that tbe_compiler correctly generates from a union declaration:
 *   - A tag enum  (Result_tag_success, Result_tag_error, Result_tag_COUNT)
 *   - Result_get_tag()
 *   - Result_set_tag()
 *   - Result_payload()
 *   - Result_payload_mut()
 */

#include <stdint.h>
#include <string.h>
#include "tinytest.h"

#include "test_union_types.h"

suite("union_types_functional") {
    describe("Result tag enum") {
        it("should have correct tag values") {
            /* Variants are indexed in declaration order, starting at 0 */
            check_int_eq(Result_tag_success, 0);
            check_int_eq(Result_tag_error,   1);
        }

        it("should have COUNT equal to variant count") {
            check_int_eq(Result_tag_COUNT, 2);
        }
    }

    describe("Result_get_tag") {
        it("should return COUNT when buf is NULL") {
            check_int_eq(Result_get_tag(NULL, 16), Result_tag_COUNT);
        }

        it("should return COUNT when size < 1") {
            uint8_t buf[16] = {0};
            check_int_eq(Result_get_tag(buf, 0), Result_tag_COUNT);
        }

        it("should read the tag byte from buf[0]") {
            uint8_t buf[16] = {0};
            buf[0] = (uint8_t)Result_tag_error;
            check_int_eq(Result_get_tag(buf, sizeof(buf)), Result_tag_error);
        }
    }

    describe("Result_set_tag") {
        it("should write tag byte to buf[0]") {
            uint8_t buf[16] = {0};
            Result_set_tag(buf, sizeof(buf), Result_tag_success);
            check_int_eq((int)buf[0], (int)Result_tag_success);
        }

        it("should be a no-op when buf is NULL") {
            /* Must not crash */
            Result_set_tag(NULL, 16, Result_tag_success);
            check(1);
        }

        it("should be a no-op when size < 1") {
            uint8_t buf[16] = {0xFF};
            Result_set_tag(buf, 0, Result_tag_success);
            /* buf[0] must remain unchanged */
            check_int_eq((int)buf[0], 0xFF);
        }
    }

    describe("Result_payload") {
        it("should return NULL when buf is NULL") {
            size_t psz = 99;
            check_null(Result_payload(NULL, 16, &psz));
        }

        it("should return NULL when size < 1") {
            uint8_t buf[16] = {0};
            size_t psz = 99;
            check_null(Result_payload(buf, 0, &psz));
        }

        it("should point one byte past buf and report size-1") {
            uint8_t buf[16] = {0};
            size_t psz = 0;
            const uint8_t *p = Result_payload(buf, sizeof(buf), &psz);
            check(p == buf + 1);
            check_uint_eq(psz, sizeof(buf) - 1);
        }

        it("should accept NULL payload_size out-param") {
            uint8_t buf[16] = {0};
            const uint8_t *p = Result_payload(buf, sizeof(buf), NULL);
            check(p == buf + 1);
        }
    }

    describe("Result_payload_mut") {
        it("should return NULL when buf is NULL") {
            size_t psz = 0;
            check_null(Result_payload_mut(NULL, 16, &psz));
        }

        it("should return NULL when size < 1") {
            uint8_t buf[16] = {0};
            check_null(Result_payload_mut(buf, 0, NULL));
        }

        it("should return writable pointer one byte past buf") {
            uint8_t buf[16] = {0};
            size_t psz = 0;
            uint8_t *p = Result_payload_mut(buf, sizeof(buf), &psz);
            check(p == buf + 1);
            check_uint_eq(psz, sizeof(buf) - 1);
            /* Verify the pointer is actually writable */
            p[0] = 0xAB;
            check_int_eq((int)buf[1], 0xAB);
        }
    }

    describe("round-trip: set tag then read payload") {
        it("should write a tag and retrieve the correct payload region") {
            uint8_t buf[32] = {0};

            Result_set_tag(buf, sizeof(buf), Result_tag_error);
            check_int_eq(Result_get_tag(buf, sizeof(buf)), Result_tag_error);

            size_t psz = 0;
            uint8_t *p = Result_payload_mut(buf, sizeof(buf), &psz);
            check(p != NULL);
            check_uint_eq(psz, sizeof(buf) - 1);

            /* Write a sentinel into the payload */
            p[0] = 0x42;
            const uint8_t *rp = Result_payload(buf, sizeof(buf), NULL);
            check_int_eq((int)rp[0], 0x42);
        }
    }
}
