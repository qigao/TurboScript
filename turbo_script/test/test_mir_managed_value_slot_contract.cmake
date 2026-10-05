if(NOT DEFINED TURBOSCRIPT_SOURCE_ROOT)
  message(FATAL_ERROR "TURBOSCRIPT_SOURCE_ROOT is required")
endif()

set(emitter
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/turbo_script_mir_value_emit.c")
set(runtime
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/turbo_script_mir_value_runtime.c")
set(compiler
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/turbo_script_mir_compiler.c")
set(api
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/turbo_script_mir_api.c")
set(functions
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/turbo_script_mir_functions.c")

file(READ "${emitter}" emitter_content)
file(READ "${runtime}" runtime_content)
file(READ "${compiler}" compiler_content)
file(READ "${api}" api_content)
file(READ "${functions}" functions_content)

foreach(marker IN ITEMS
    "ts_mir_new_managed_slot(c, sizeof(exprtk_value_t))"
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

foreach(marker IN ITEMS
    "MIR_reg_t ts_mir_new_managed_slot"
    "void ts_emit_managed_slot_prologue"
    "MIR_prepend_insn"
    "MIR_ALLOCA")
  string(FIND "${compiler_content}" "${marker}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "managed value slot compiler contract missing: ${marker}")
  endif()
endforeach()

string(FIND "${api_content}"
  "ts_emit_managed_slot_prologue(&compiler)" top_level_flush)
if(top_level_flush EQUAL -1)
  message(FATAL_ERROR "top-level managed slot prologue flush missing")
endif()

string(FIND "${functions_content}"
  "ts_emit_managed_slot_prologue(c)" function_flush)
if(function_flush EQUAL -1)
  message(FATAL_ERROR "script-function managed slot prologue flush missing")
endif()

string(FIND "${emitter_content}" "MIR_ALLOCA" inline_alloca)
if(NOT inline_alloca EQUAL -1)
  message(FATAL_ERROR
    "managed value expression must not allocate storage inside loop/body path")
endif()

message(STATUS
  "TurboScript MIR compiler-owned managed value slot contract verified")
