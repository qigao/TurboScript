#ifndef EXPRTK_TYPED_ARRAY_INTERNAL_H
#define EXPRTK_TYPED_ARRAY_INTERNAL_H

#include "exprtk_types.h"

static inline size_t exprtk_typed_array_element_size(exprtk_typed_array_kind_t kind) {
    switch (kind) {
#define EXPRTK_ARRAY_ELEMENT_SIZE(kind_value, abi_value, storage_type, kind_name) \
        case kind_value: return sizeof(storage_type);
        Replay(EXPRTK_TYPED_ARRAY_KIND_SCHEMA, EXPRTK_ARRAY_ELEMENT_SIZE)
#undef EXPRTK_ARRAY_ELEMENT_SIZE
        default: return 0;
    }
}

static inline const char *exprtk_typed_array_kind_name(exprtk_typed_array_kind_t kind) {
    switch (kind) {
#define EXPRTK_ARRAY_KIND_NAME(kind_value, abi_value, storage_type, kind_name) \
        case kind_value: return kind_name;
        Replay(EXPRTK_TYPED_ARRAY_KIND_SCHEMA, EXPRTK_ARRAY_KIND_NAME)
#undef EXPRTK_ARRAY_KIND_NAME
        default: return "";
    }
}

/* Validate before allocation or copying. Failure leaves the output untouched;
 * zero elements still require a supported storage kind. No storage is owned. */
static inline int exprtk_typed_array_byte_size(exprtk_typed_array_kind_t kind,
                                               size_t count, size_t *out_bytes) {
    size_t element_size = exprtk_typed_array_element_size(kind);
    if (!out_bytes || element_size == 0 || count > SIZE_MAX / element_size) return -1;
    *out_bytes = count * element_size;
    return 0;
}

#endif
