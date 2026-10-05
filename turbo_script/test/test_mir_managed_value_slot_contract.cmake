if(NOT DEFINED TURBOSCRIPT_SOURCE_ROOT)
  message(FATAL_ERROR "TURBOSCRIPT_SOURCE_ROOT is required")
endif()

set(emitter
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/turbo_script_mir_value_emit.c")
set(runtime
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/turbo_script_mir_value_runtime.c")

file(READ "${emitter}" emitter_content)
file(READ "${runtime}" runtime_content)

foreach(marker IN ITEMS
    "MIR_ALLOCA"
    "sizeof(exprtk_value_t)"
    "value_expr_proto"
    "value_slot_destroy_proto")
  string(FIND "${emitter_content}" "${marker}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "managed value slot emitter contract missing: ${marker}")
  endif()
endforeach()

foreach(marker IN ITEMS
    "exprtk_value_t *out_value"
    "cmeta_data_value_init_zero(value_data, out_value)"
    "cmeta_data_value_move(value_data, out_value, &result)"
    "void ts_mir_value_slot_destroy(void *value)"
    "cmeta_data_value_destroy(exprtk_value_cmeta_data(), value)")
  string(FIND "${runtime_content}" "${marker}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "managed value slot runtime contract missing: ${marker}")
  endif()
endforeach()

message(STATUS "TurboScript MIR compiler-owned managed value slot contract verified")
