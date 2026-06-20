#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tbe_wire.h"

/**
 * @brief Helper used by JIT-compiled MIR code to extract a variable-length string.
 * TurboNet TBE variable data uses a 4-byte little-endian length prefix.
 */
char* tbe_read_varstring(const uint8_t* buf, size_t offset, size_t buf_remaining) {
    tbe_var_data_t value = {0};
    char* s;

    if (!tbe_wire_read_var_data(buf + offset, buf_remaining, 0, &value)) {
        return NULL;
    }

    s = (char*)malloc(value.size + 1);
    if (!s) return NULL;

    if (value.size > 0) {
        memcpy(s, value.data, value.size);
    }
    s[value.size] = '\0';
    return s;
}
