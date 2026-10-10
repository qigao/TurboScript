#ifndef EXPRTK_VALUE_SCHEMA_H
#define EXPRTK_VALUE_SCHEMA_H

#include <cmeta/pp.h>

/* Native value tags are ABI data. Keep their numbers independent of row order.
 * Two sequential batches respect CMeta's 16-row expansion limit. The last
 * column is the diagnostic name; language-level typeof policy is separate. */
#define EXPRTK_VALUE_TAG_SCHEMA(M) \
    Schema(M, \
        (EXPRTK_VAL_NUMBER, 0, "number"), \
        (EXPRTK_VAL_STRING, 1, "string"), \
        (EXPRTK_VAL_INTEGER, 2, "int64"), \
        (EXPRTK_VAL_BOOL, 3, "bool"), \
        (EXPRTK_VAL_BYTES, 4, "bytes"), \
        (EXPRTK_VAL_VECTOR, 5, "vector"), \
        (EXPRTK_VAL_MAP, 6, "map"), \
        (EXPRTK_VAL_OBJECT, 7, "object"), \
        (EXPRTK_VAL_NULL, 8, "null"), \
        (EXPRTK_VAL_LIST, 9, "list"), \
        (EXPRTK_VAL_FUNCTION, 10, "function"), \
        (EXPRTK_VAL_COROUTINE, 11, "unknown"), \
        (EXPRTK_VAL_CLASS, 12, "class"), \
        (EXPRTK_VAL_INSTANCE, 13, "instance"), \
        (EXPRTK_VAL_BOUND_METHOD, 14, "bound_method"), \
        (EXPRTK_VAL_UUID, 15, "uuid")) \
    Schema(M, \
        (EXPRTK_VAL_DATETIME, 16, "datetime"), \
        (EXPRTK_VAL_DATE, 17, "date"), \
        (EXPRTK_VAL_TIME, 18, "time"), \
        (EXPRTK_VAL_DURATION, 19, "duration"), \
        (EXPRTK_VAL_DECIMAL, 20, "decimal"), \
        (EXPRTK_VAL_BIGINT, 21, "bigint"), \
        (EXPRTK_VAL_MONEY, 22, "money"), \
        (EXPRTK_VAL_ENUM, 23, "enum"), \
        (EXPRTK_VAL_FLAGS, 24, "flags"), \
        (EXPRTK_VAL_SET, 25, "set"), \
        (EXPRTK_VAL_OFFSET_DATETIME, 26, "offset_datetime"), \
        (EXPRTK_VAL_TYPED_ARRAY, 27, "typed_array"))

/* Direct-assignment payloads generate storage and borrowed/value constructors.
 * Normalization, composite construction and resource management remain with
 * their existing owners. These rows do not grant CMeta VALUE lifecycle rights. */
#define EXPRTK_DIRECT_VALUE_SCHEMA(M) \
    Schema(M, \
        (EXPRTK_VAL_NUMBER, double, number, exprtk_val_num), \
        (EXPRTK_VAL_STRING, vstr, string, exprtk_val_str), \
        (EXPRTK_VAL_INTEGER, int64_t, integer, exprtk_val_int), \
        (EXPRTK_VAL_BYTES, vstr, bytes, exprtk_val_bytes), \
        (EXPRTK_VAL_UUID, cmeta_uuid_t, uuid, exprtk_val_uuid), \
        (EXPRTK_VAL_DATETIME, datetime_t, datetime, exprtk_val_datetime), \
        (EXPRTK_VAL_DATE, exprtk_date_t, date, exprtk_val_date), \
        (EXPRTK_VAL_TIME, exprtk_time_t, time, exprtk_val_time), \
        (EXPRTK_VAL_DURATION, int64_t, duration_ms, exprtk_val_duration), \
        (EXPRTK_VAL_DECIMAL, exprtk_decimal_t, decimal, exprtk_val_decimal), \
        (EXPRTK_VAL_MONEY, exprtk_money_t, money, exprtk_val_money))

/* Typed-array storage kinds are a separate ABI from script value tags.
 * Numeric conversion/boxing policy remains in the runtime adapters. */
#define EXPRTK_TYPED_ARRAY_KIND_SCHEMA(M) \
    Schema(M, \
        (EXPRTK_TYPED_I32, 1, int32_t, "i32"), \
        (EXPRTK_TYPED_I64, 2, int64_t, "i64"), \
        (EXPRTK_TYPED_F32, 3, float, "f32"), \
        (EXPRTK_TYPED_F64, 4, double, "f64"))

#endif
