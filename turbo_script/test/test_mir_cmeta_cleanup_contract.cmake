if(NOT DEFINED TURBOSCRIPT_SOURCE_ROOT)
  message(FATAL_ERROR "TURBOSCRIPT_SOURCE_ROOT is required")
endif()

file(GLOB MIR_RUNTIME_SOURCES
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/mir/*.c")
list(APPEND MIR_RUNTIME_SOURCES
  "${TURBOSCRIPT_SOURCE_ROOT}/turbo_script/src/turbo_script_mir.c")

set(canonical_uses 0)
foreach(source IN LISTS MIR_RUNTIME_SOURCES)
  file(READ "${source}" content)
  string(FIND "${content}" "exprtk_value_destroy(" direct_destroy)
  if(NOT direct_destroy EQUAL -1)
    message(FATAL_ERROR
      "MIR source bypasses canonical ExprTk value lifecycle: ${source}")
  endif()

  string(REGEX MATCHALL
    "cmeta_data_value_destroy\\(exprtk_value_cmeta_data\\(\\),"
    matches "${content}")
  list(LENGTH matches match_count)
  math(EXPR canonical_uses "${canonical_uses} + ${match_count}")
endforeach()

if(canonical_uses LESS 1)
  message(FATAL_ERROR
    "MIR sources do not consume exprtk_value_cmeta_data() lifecycle")
endif()

message(STATUS
  "MIR canonical ExprTk value lifecycle contract: ${canonical_uses} cleanup sites")
