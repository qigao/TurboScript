# Helper macro: build one turbo_script test binary and register it as a CTest.
# Each binary gets the plugin .tbs files copied to its output directory.
macro(add_ts_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS turbo_script TurboUtils::TinyTest TurboNet::CoroNet exprtk ta mir_static
    INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
    DEPENDS mapper_tbs fin_tbs ta_tbs ts_tbs parser_tbs net_tbs coro_tbs
    FOLDER turbo_script)
  add_custom_command(
    TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fin_tbs>    $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ta_tbs>     $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ts_tbs>     $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:parser_tbs> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mapper_tbs> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:net_tbs>    $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:coro_tbs>   $<TARGET_FILE_DIR:${TARGET_NAME}>)
endmacro()

# Interpreter test suites (split by module)
add_ts_test(test_turbo_script_basics     test_turbo_script_basics.c)
add_ts_test(test_turbo_script_quant      test_turbo_script_quant.c)
add_ts_test(test_turbo_script_scientific test_turbo_script_scientific.c)
add_ts_test(test_turbo_script_io         test_turbo_script_io.c)
add_ts_test(test_turbo_script_timer      test_turbo_script_timer.c)
add_ts_test(test_turbo_script_task       test_turbo_script_task.c)
add_ts_test(test_turbo_script_coro       test_turbo_script_coro.c)
cmake_add_test(
  TARGET test_turbo_script_host_result
  SOURCES test_turbo_script_host_result.c
  LIBS turbo_script_lib TurboUtils::TinyTest exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_registry
  SOURCES test_turbo_script_host_registry.c
  LIBS turbo_script_lib TurboUtils::TinyTest exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_module
  SOURCES test_turbo_script_host_module.c
  LIBS turbo_script_lib TurboUtils::TinyTest exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_instance
  SOURCES test_turbo_script_host_instance.c
  LIBS turbo_script_lib TurboUtils::TinyTest exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  SOURCES bench_turbo_script.c
  LIBS turbo_script_lib TurboUtils::TinyTest exprtk mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
  FOLDER turbo_script)

# MIR / JIT test suites (split by module)
macro(add_mir_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS turbo_script TurboUtils::TinyTest exprtk mir_static
    INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
    DEPENDS mapper_tbs
    FOLDER turbo_script)
  add_custom_command(
    TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:mapper_tbs> $<TARGET_FILE_DIR:${TARGET_NAME}>)
endmacro()

add_mir_test(test_turbo_script_mir        test_turbo_script_mir.c)
add_mir_test(test_turbo_script_mir_core     test_turbo_script_mir_core.c)
add_mir_test(test_turbo_script_mir_oop      test_turbo_script_mir_oop.c)
add_mir_test(test_turbo_script_mir_advanced test_turbo_script_mir_advanced.c)

# JIT Performance Benchmarks
cmake_add_benchmark(
  SOURCES bench_turbo_script_mir.c
  LIBS turbo_script_lib exprtk mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
  FOLDER turbo_script)
