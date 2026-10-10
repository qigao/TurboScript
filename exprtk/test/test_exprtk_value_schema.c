#include "exprtk.h"
#include "exprtk_module.h"
#include "exprtk_typed_array_internal.h"
#include "tinytest.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The C++ TU consumes the same installed-facing headers independently. */
extern exprtk_value_t exprtk_test_cpp_integer(int64_t value);
extern exprtk_value_t exprtk_test_cpp_string(vstr value);
extern size_t exprtk_test_cpp_value_size(void);
extern size_t exprtk_test_cpp_payload_offset(void);
extern const char *type_name(int type);
extern exprtk_builtin_fn exprtk_registry_find(const char *name);

suite("exprtk value declaration compatibility") {
    it("preserves every native value tag number") {
        const exprtk_value_type_t tags[] = {
            EXPRTK_VAL_NUMBER, EXPRTK_VAL_STRING, EXPRTK_VAL_INTEGER,
            EXPRTK_VAL_BOOL, EXPRTK_VAL_BYTES, EXPRTK_VAL_VECTOR,
            EXPRTK_VAL_MAP, EXPRTK_VAL_OBJECT, EXPRTK_VAL_NULL,
            EXPRTK_VAL_LIST, EXPRTK_VAL_FUNCTION, EXPRTK_VAL_COROUTINE,
            EXPRTK_VAL_CLASS, EXPRTK_VAL_INSTANCE, EXPRTK_VAL_BOUND_METHOD,
            EXPRTK_VAL_UUID, EXPRTK_VAL_DATETIME, EXPRTK_VAL_DATE,
            EXPRTK_VAL_TIME, EXPRTK_VAL_DURATION, EXPRTK_VAL_DECIMAL,
            EXPRTK_VAL_BIGINT, EXPRTK_VAL_MONEY, EXPRTK_VAL_ENUM,
            EXPRTK_VAL_FLAGS, EXPRTK_VAL_SET, EXPRTK_VAL_OFFSET_DATETIME,
            EXPRTK_VAL_TYPED_ARRAY
        };
        for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i)
            check_equal((int)tags[i], (int)i);
    }

    it("keeps diagnostic names including unknown and bound method policy") {
        const char *names[] = {
            "number", "string", "int64", "bool", "bytes", "vector",
            "map", "object", "null", "list", "function", "unknown",
            "class", "instance", "bound_method", "uuid", "datetime",
            "date", "time", "duration", "decimal", "bigint", "money",
            "enum", "flags", "set", "offset_datetime", "typed_array"
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            check_equal(type_name((int)i), names[i]);
        check_equal(type_name(-1), "unknown");
        check_equal(type_name(28), "unknown");
    }

    it("preserves payloads and borrowed state in mechanical constructors") {
        char text[] = {'a', '\0', 'z'};
        vstr view = {text, sizeof(text)};
        cmeta_uuid_t uuid = {0};
        datetime_t datetime = {0};
        uuid.bytes[0] = 0x12;
        uuid.bytes[SALTS_UUID_SIZE - 1] = 0xab;
        datetime.year = 2026;
        datetime.tz_offset = 480;
        exprtk_date_t date = {2026, 10, 10};
        exprtk_time_t time = {12, 34, 56, 789};
        exprtk_decimal_t decimal = {-12345, 3};
        exprtk_money_t money = {{1250, 2}, "USD"};
        exprtk_value_t values[] = {
            exprtk_val_num(-1.25), exprtk_val_str(view), exprtk_val_int(INT64_MIN),
            exprtk_val_bytes(view), exprtk_val_uuid(uuid), exprtk_val_datetime(datetime),
            exprtk_val_date(date), exprtk_val_time(time), exprtk_val_duration(INT64_MAX),
            exprtk_val_decimal(decimal), exprtk_val_money(money)
        };
        const int tags[] = {0, 1, 2, 4, 15, 16, 17, 18, 19, 20, 22};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            check_equal((int)values[i].type, tags[i]);
            check_equal((int)values[i].ownership, (int)EXPRTK_VALUE_BORROWED);
            check_null(values[i].storage);
            check_null(values[i].storage_aux);
        }
        check_equal(values[0].data.number, -1.25);
        check(values[1].data.string.data == text);
        check_equal(values[1].data.string.len, sizeof(text));
        check_equal(values[2].data.integer, INT64_MIN);
        check(values[3].data.bytes.data == text);
        check_equal(values[3].data.bytes.len, sizeof(text));
        check_equal(values[4].data.uuid.bytes[0], (uint8_t)0x12);
        check_equal(values[4].data.uuid.bytes[SALTS_UUID_SIZE - 1], (uint8_t)0xab);
        check_equal(values[5].data.datetime.year, 2026);
        check_equal(values[5].data.datetime.tz_offset, 480);
        check_equal(values[6].data.date.year, 2026);
        check_equal(values[7].data.time.millisecond, 789);
        check_equal(values[8].data.duration_ms, INT64_MAX);
        check_equal(values[9].data.decimal.mantissa, (int64_t)-12345);
        check_equal(values[9].data.decimal.scale, 3);
        check_equal(values[10].data.money.currency, "USD");
        check_equal(exprtk_val_bool(-8).data.boolean, 1);
        check_equal(exprtk_val_bool(0).data.boolean, 0);
    }

    it("passes values between independently compiled C11 and C++17 code") {
        char text[] = {'x', '\0', 'y'};
        vstr view = {text, sizeof(text)};
        exprtk_value_t integer = exprtk_test_cpp_integer(INT64_MAX);
        exprtk_value_t string = exprtk_test_cpp_string(view);
        check_equal(exprtk_test_cpp_value_size(), sizeof(exprtk_value_t));
        check_equal(exprtk_test_cpp_payload_offset(), offsetof(exprtk_value_t, data));
        check_equal((int)integer.type, 2);
        check_equal(integer.data.integer, INT64_MAX);
        check_equal((int)string.type, 1);
        check(string.data.string.data == text);
        check_equal(string.data.string.len, sizeof(text));
        check_null(string.storage);
    }

    group("language type names") {
        static exprtk_env_t env;
        before_each() { exprtk_env_init(&env); }
        after_each() { exprtk_env_free(&env); }

        it("keeps typeof policy separate from diagnostic names") {
            exprtk_builtin_fn function = exprtk_registry_find("typeof");
            exprtk_value_t input = {0};
            check_not_null(function);
            input.type = EXPRTK_VAL_BOUND_METHOD;
            exprtk_value_t result = function(1, &input, &env, &env.arena);
            check_equal((int)result.type, (int)EXPRTK_VAL_STRING);
            check(vstr_eq(result.data.string, vstr_from_cstr("function")));
            input.type = EXPRTK_VAL_COROUTINE;
            result = function(1, &input, &env, &env.arena);
            check(vstr_eq(result.data.string, vstr_from_cstr("unknown")));
            result = function(0, NULL, &env, &env.arena);
            check(vstr_eq(result.data.string, vstr_from_cstr("null")));
        }
    }

    group("typed array storage") {
        static exprtk_env_t env;
        static exprtk_value_t copies[4];
        static exprtk_node_t *root;
        before_each() {
            exprtk_env_init(&env);
            memset(copies, 0, sizeof(copies));
            root = NULL;
        }
        after_each() {
            if (root) exprtk_free(root);
            for (size_t i = 0; i < 4; ++i) exprtk_value_destroy(&copies[i]);
            exprtk_env_free(&env);
        }

        it("preserves native kind tags and rejects invalid byte extents") {
            const exprtk_typed_array_kind_t kinds[] = {
                EXPRTK_TYPED_I32, EXPRTK_TYPED_I64, EXPRTK_TYPED_F32, EXPRTK_TYPED_F64
            };
            const size_t widths[] = {sizeof(int32_t), sizeof(int64_t), sizeof(float), sizeof(double)};
            const char *names[] = {"i32", "i64", "f32", "f64"};
            for (size_t i = 0; i < 4; ++i) {
                size_t bytes = 123;
                size_t limit = SIZE_MAX / widths[i];
                check_equal((int)kinds[i], (int)i + 1);
                check_equal(exprtk_typed_array_kind_name(kinds[i]), names[i]);
                check_equal(exprtk_typed_array_byte_size(kinds[i], 0, &bytes), 0);
                check_equal(bytes, (size_t)0);
                check_equal(exprtk_typed_array_byte_size(kinds[i], limit, &bytes), 0);
                check_equal(bytes, limit * widths[i]);
                check_equal(exprtk_typed_array_byte_size(kinds[i], limit + 1, &bytes), -1);
                check_equal(bytes, limit * widths[i]);
                check_equal(exprtk_typed_array_byte_size(kinds[i], 1, NULL), -1);
            }
            size_t bytes = 123;
            check_equal(exprtk_typed_array_byte_size((exprtk_typed_array_kind_t)0, 0, &bytes), -1);
            check_equal(exprtk_typed_array_byte_size((exprtk_typed_array_kind_t)5, 1, &bytes), -1);
            check_equal(bytes, (size_t)123);
            check_equal(exprtk_typed_array_kind_name((exprtk_typed_array_kind_t)0), "");
        }

        it("copies every storage kind without retaining the borrowed source") {
            int32_t i32[] = {INT32_MIN, INT32_MAX};
            int64_t i64[] = {INT64_MIN, INT64_MAX};
            float f32[] = {-1.25f, 2.5f};
            double f64[] = {-3.5, 4.75};
            exprtk_value_t sources[] = {
                exprtk_val_typed_array(EXPRTK_TYPED_I32, i32, 2, 0),
                exprtk_val_typed_array(EXPRTK_TYPED_I64, i64, 2, 0),
                exprtk_val_typed_array(EXPRTK_TYPED_F32, f32, 2, 0),
                exprtk_val_typed_array(EXPRTK_TYPED_F64, f64, 2, 0)
            };
            for (size_t i = 0; i < 4; ++i) {
                check_equal(exprtk_value_copy_to_env(sources[i], &env, &copies[i]), 0);
                check_not_null(copies[i].storage);
                check(copies[i].data.typed_array.data != sources[i].data.typed_array.data);
                check_equal(copies[i].data.typed_array.count, (size_t)2);
                check_equal(copies[i].data.typed_array.heap_owned, 0);
                check_equal((int)copies[i].ownership, (int)EXPRTK_VALUE_OWNED);
            }
            i32[0] = 0;
            i64[1] = 0;
            f32[0] = 0;
            f64[1] = 0;
            check_equal(exprtk_typed_array_get_value(copies[0], 0).data.integer, (int64_t)INT32_MIN);
            check_equal(exprtk_typed_array_get_value(copies[1], 1).data.integer, INT64_MAX);
            check_equal(exprtk_typed_array_get_value(copies[2], 0).data.number, -1.25);
            check_equal(exprtk_typed_array_get_value(copies[3], 1).data.number, 4.75);
            check_equal(exprtk_typed_array_get_value(copies[3], 2).data.number, 0.0);
        }

        it("leaves the output unchanged when a storage kind is unsupported") {
            exprtk_value_t output = exprtk_val_int(42);
            exprtk_value_t source = exprtk_val_typed_array((exprtk_typed_array_kind_t)0, NULL, 0, 0);
            check_equal(exprtk_value_copy_to_env(source, &env, &output), -1);
            check_equal((int)output.type, (int)EXPRTK_VAL_INTEGER);
            check_equal(output.data.integer, (int64_t)42);
        }

        it("preserves script creation, names and reads for all four storage kinds") {
            const char *script =
                "a = typed.i32(-2, 3); b = typed.i64(-4, 5);"
                "c = typed.f32(-1.25, 2.5); d = typed.f64(-3.5, 4.75);"
                "(a.kind == \"i32\") + (b.kind() == \"i64\") +"
                "(c.kind == \"f32\") + (d.kind() == \"f64\") +"
                "(a[0] == -2) + (b[1] == 5) + (c[0] == -1.25) + (d[1] == 4.75)";
            root = exprtk_parse(script, 0);
            check_not_null(root);
            exprtk_value_t result = exprtk_eval(root, &env);
            check_equal(env.aborted, 0);
            check_equal((int)result.type, (int)EXPRTK_VAL_NUMBER);
            check_equal(result.data.number, 8.0);
        }
    }
}
