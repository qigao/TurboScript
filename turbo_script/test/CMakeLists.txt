# Helper macro: build one turbo_script test binary and register it as a CTest.
# Each binary gets the plugin .tbs files copied to its output directory.
macro(add_ts_test TARGET_NAME SOURCE_FILE)
  add_executable(${TARGET_NAME} ${SOURCE_FILE})
  target_link_libraries(${TARGET_NAME} PRIVATE turbo_script TurboUtils::TinyTest
                                               exprtk ta mir_static)
  add_dependencies(${TARGET_NAME} fin_tbs ta_tbs ts_tbs parser_tbs net_tbs)
  add_custom_command(
    TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fin_tbs>    $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ta_tbs>     $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ts_tbs>     $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:parser_tbs> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:net_tbs>    $<TARGET_FILE_DIR:${TARGET_NAME}>)
  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
  add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
  set_target_properties(${TARGET_NAME} PROPERTIES FOLDER turbo_script)
endmacro()

# Interpreter test suites (split by module)
add_ts_test(test_turbo_script_basics     test_turbo_script_basics.c)
add_ts_test(test_turbo_script_quant      test_turbo_script_quant.c)
add_ts_test(test_turbo_script_scientific test_turbo_script_scientific.c)
add_ts_test(test_turbo_script_io         test_turbo_script_io.c)

add_executable(bench_turbo_script bench_turbo_script.c)
target_link_libraries(
  bench_turbo_script PRIVATE turbo_script_lib TurboUtils::TinyTest exprtk
                             mir_static)
target_include_directories(bench_turbo_script
                           PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
add_test(NAME bench_turbo_script COMMAND bench_turbo_script)
set_target_properties(bench_turbo_script PROPERTIES FOLDER turbo_script)

# MIR / JIT test suites (split by module)
macro(add_mir_test TARGET_NAME SOURCE_FILE)
  add_executable(${TARGET_NAME} ${SOURCE_FILE})
  target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
  target_link_libraries(${TARGET_NAME} PRIVATE turbo_script TurboUtils::TinyTest exprtk mir_static)
  add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
  set_target_properties(${TARGET_NAME} PROPERTIES FOLDER turbo_script)
endmacro()

add_mir_test(test_turbo_script_mir_core     test_turbo_script_mir_core.c)
add_mir_test(test_turbo_script_mir_oop      test_turbo_script_mir_oop.c)
add_mir_test(test_turbo_script_mir_advanced test_turbo_script_mir_advanced.c)

# JIT Performance Benchmarks
add_executable(bench_turbo_script_mir bench_turbo_script_mir.c)
target_include_directories(bench_turbo_script_mir
                           PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
target_link_libraries(bench_turbo_script_mir PRIVATE turbo_script_lib exprtk
                                                     mir_static)
set_target_properties(bench_turbo_script_mir PROPERTIES FOLDER turbo_script)
