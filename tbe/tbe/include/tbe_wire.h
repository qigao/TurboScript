#ifndef TURBONET_SBE_WIRE_H
#define TURBONET_SBE_WIRE_H

#include "tbe_endian.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct tbe_var_data_s {
    const uint8_t *data;
    size_t size;
} tbe_var_data_t;

static inline uint8_t tbe_wire_read_u8(const uint8_t *data, int is_big_endian) {
    uint8_t value = 0;
    if (!data) return 0;  // Add null check
    (void)is_big_endian;
    memcpy(&value, data, sizeof(value));
    return value;
}

static inline int8_t tbe_wire_read_i8(const uint8_t *data, int is_big_endian) {
    int8_t value = 0;
    if (!data) return 0;  // Add null check
    (void)is_big_endian;
    memcpy(&value, data, sizeof(value));
    return value;
}

static inline uint16_t tbe_wire_read_u16(const uint8_t *data, int is_big_endian) {
    uint16_t value = 0;
    if (!data) return 0;  // Add null check
    memcpy(&value, data, sizeof(value));
    return is_big_endian ? tbe_be16toh(value) : tbe_le16toh(value);
}

static inline int16_t tbe_wire_read_i16(const uint8_t *data, int is_big_endian) {
    if (!data) return 0;  // Add null check
    uint16_t raw = tbe_wire_read_u16(data, is_big_endian);
    int16_t value = 0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline uint32_t tbe_wire_read_u32(const uint8_t *data, int is_big_endian) {
    uint32_t value = 0;
    if (!data) return 0;  // Add null check
    memcpy(&value, data, sizeof(value));
    return is_big_endian ? tbe_be32toh(value) : tbe_le32toh(value);
}

static inline int32_t tbe_wire_read_i32(const uint8_t *data, int is_big_endian) {
    if (!data) return 0;  // Add null check
    uint32_t raw = tbe_wire_read_u32(data, is_big_endian);
    int32_t value = 0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline uint64_t tbe_wire_read_u64(const uint8_t *data, int is_big_endian) {
    uint64_t value = 0;
    if (!data) return 0;  // Add null check
    memcpy(&value, data, sizeof(value));
    return is_big_endian ? tbe_be64toh(value) : tbe_le64toh(value);
}

static inline int64_t tbe_wire_read_i64(const uint8_t *data, int is_big_endian) {
    if (!data) return 0;  // Add null check
    uint64_t raw = tbe_wire_read_u64(data, is_big_endian);
    int64_t value = 0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline float tbe_wire_read_f32(const uint8_t *data, int is_big_endian) {
    if (!data) return 0.0f;  // Add null check
    uint32_t raw = tbe_wire_read_u32(data, is_big_endian);
    float value = 0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline double tbe_wire_read_f64(const uint8_t *data, int is_big_endian) {
    if (!data) return 0.0;  // Add null check
    uint64_t raw = tbe_wire_read_u64(data, is_big_endian);
    double value = 0;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline void tbe_wire_write_u8(uint8_t *data, int is_big_endian, uint8_t value) {
    if (!data) return;  // Add null check
    (void)is_big_endian;
    memcpy(data, &value, sizeof(value));
}

static inline void tbe_wire_write_i8(uint8_t *data, int is_big_endian, int8_t value) {
    if (!data) return;  // Add null check
    (void)is_big_endian;
    memcpy(data, &value, sizeof(value));
}

static inline void tbe_wire_write_u16(uint8_t *data, int is_big_endian, uint16_t value) {
    if (!data) return;  // Add null check
    uint16_t raw = is_big_endian ? tbe_htobe16(value) : tbe_htole16(value);
    memcpy(data, &raw, sizeof(raw));
}

static inline void tbe_wire_write_i16(uint8_t *data, int is_big_endian, int16_t value) {
    if (!data) return;  // Add null check
    uint16_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    tbe_wire_write_u16(data, is_big_endian, raw);
}

static inline void tbe_wire_write_u32(uint8_t *data, int is_big_endian, uint32_t value) {
    if (!data) return;  // Add null check
    uint32_t raw = is_big_endian ? tbe_htobe32(value) : tbe_htole32(value);
    memcpy(data, &raw, sizeof(raw));
}

static inline void tbe_wire_write_i32(uint8_t *data, int is_big_endian, int32_t value) {
    if (!data) return;  // Add null check
    uint32_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    tbe_wire_write_u32(data, is_big_endian, raw);
}

static inline void tbe_wire_write_u64(uint8_t *data, int is_big_endian, uint64_t value) {
    if (!data) return;  // Add null check
    uint64_t raw = is_big_endian ? tbe_htobe64(value) : tbe_htole64(value);
    memcpy(data, &raw, sizeof(raw));
}

static inline void tbe_wire_write_i64(uint8_t *data, int is_big_endian, int64_t value) {
    if (!data) return;  // Add null check
    uint64_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    tbe_wire_write_u64(data, is_big_endian, raw);
}

static inline void tbe_wire_write_f32(uint8_t *data, int is_big_endian, float value) {
    if (!data) return;  // Add null check
    uint32_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    tbe_wire_write_u32(data, is_big_endian, raw);
}

static inline void tbe_wire_write_f64(uint8_t *data, int is_big_endian, double value) {
    if (!data) return;  // Add null check
    uint64_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    tbe_wire_write_u64(data, is_big_endian, raw);
}

static inline bool tbe_wire_write_var_data(uint8_t *data,
                                           size_t size,
                                           int is_big_endian,
                                           const void *value_data,
                                           size_t value_size) {
    if (!data || size < sizeof(uint32_t) || value_size > (size_t)UINT32_MAX) {
        return false;
    }

    if (value_size > 0 && !value_data) {
        return false;
    }

    if (size < sizeof(uint32_t) + value_size) {
        return false;
    }

    tbe_wire_write_u32(data, is_big_endian, (uint32_t)value_size);
    if (value_size > 0) {
        memcpy(data + sizeof(uint32_t), value_data, value_size);
    }

    return true;
}

static inline bool tbe_wire_read_var_data(const uint8_t *data,
                                          size_t size,
                                          int is_big_endian,
                                          tbe_var_data_t *out) {
    uint32_t length = 0;

    /* Current DSL uses a uint32 length prefix for variable data fields. */

    if (!data || !out || size < sizeof(uint32_t)) {
        if (out) {
            out->data = NULL;
            out->size = 0;
        }
        return false;
    }

    length = tbe_wire_read_u32(data, is_big_endian);
    
    // Check for overflow and reasonable bounds
    if ((size_t)length > size - sizeof(uint32_t)) {
        out->data = NULL;
        out->size = 0;
        return false;
    }
    
    // Additional sanity check: prevent unreasonably large allocations
    if (length > 100 * 1024 * 1024) {  // 100MB limit
        out->data = NULL;
        out->size = 0;
        return false;
    }

    out->data = data + sizeof(uint32_t);
    out->size = (size_t)length;
    return true;
}

static inline const uint8_t *tbe_wire_var_data_end(const tbe_var_data_t *value) {
    if (!value || !value->data) {
        return NULL;
    }

    return value->data + value->size;
}

#endif /* TURBONET_SBE_WIRE_H */
