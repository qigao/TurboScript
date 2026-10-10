#include "exprtk_module.h"
#include "ts_plugin.h"
#include <cstddef>

extern "C" exprtk_value_t exprtk_test_cpp_integer(int64_t value) {
    return exprtk_val_int(value);
}

extern "C" exprtk_value_t exprtk_test_cpp_string(vstr value) {
    return exprtk_val_str(value);
}

extern "C" size_t exprtk_test_cpp_value_size(void) {
    return sizeof(exprtk_value_t);
}

extern "C" size_t exprtk_test_cpp_payload_offset(void) {
    return offsetof(exprtk_value_t, data);
}
