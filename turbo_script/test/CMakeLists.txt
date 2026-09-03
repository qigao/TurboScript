# Helper macro: build one turbo_script test binary and register it as a CTest.
# Each binary gets the plugin .tbs files copied to its output directory.
add_library(test_plugin_loader_fixture SHARED plugin_loader_fixture.c)
cmake_config_target(
  test_plugin_loader_fixture
  FOLDER turbo_script
  OUTPUT_NAME loader_fixture
  PLUGIN)
target_include_directories(
  test_plugin_loader_fixture PRIVATE ${CMAKE_SOURCE_DIR}/exprtk/include)
target_link_libraries(test_plugin_loader_fixture PRIVATE Salts::Core)
set_target_properties(
  test_plugin_loader_fixture
  PROPERTIES PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)

function(add_plugin_loader_fixture target output_name)
  add_library(${target} SHARED plugin_loader_fixture.c)
  cmake_config_target(
    ${target}
    FOLDER turbo_script
    OUTPUT_NAME ${output_name}
    PLUGIN)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/exprtk/include)
  target_link_libraries(${target} PRIVATE Salts::Core)
  set_target_properties(
    ${target}
    PROPERTIES PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
               LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)
endfunction()

add_plugin_loader_fixture(test_plugin_loader_prefixed_conflict tbs_loader_fixture)
target_compile_definitions(
  test_plugin_loader_prefixed_conflict PRIVATE FIXTURE_PLUGIN_NAME="different_name")

add_plugin_loader_fixture(test_plugin_loader_bad_abi loader_bad_abi)
target_compile_definitions(
  test_plugin_loader_bad_abi
  PRIVATE FIXTURE_PLUGIN_NAME="loader_bad_abi" FIXTURE_ABI_VERSION=999)

add_plugin_loader_fixture(test_plugin_loader_wrong_name loader_wrong_name)
target_compile_definitions(
  test_plugin_loader_wrong_name PRIVATE FIXTURE_PLUGIN_NAME="different_name")

add_plugin_loader_fixture(test_plugin_loader_init_failure loader_init_failure)
target_compile_definitions(
  test_plugin_loader_init_failure
  PRIVATE FIXTURE_PLUGIN_NAME="loader_init_failure" FIXTURE_INIT_FAILURE)

add_plugin_loader_fixture(test_plugin_loader_legacy loader_legacy)
target_compile_definitions(
  test_plugin_loader_legacy PRIVATE FIXTURE_PLUGIN_NAME="loader_legacy")
set_target_properties(
  test_plugin_loader_legacy
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_plugin_loader_fixture(test_plugin_loader_cwd_only loader_cwd_only)
target_compile_definitions(
  test_plugin_loader_cwd_only PRIVATE FIXTURE_PLUGIN_NAME="loader_cwd_only")
set_target_properties(
  test_plugin_loader_cwd_only
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_library(test_plugin_loader_missing_entry SHARED plugin_loader_missing_entry.c)
cmake_config_target(
  test_plugin_loader_missing_entry
  FOLDER turbo_script
  OUTPUT_NAME loader_missing_entry
  PLUGIN)
target_include_directories(
  test_plugin_loader_missing_entry PRIVATE ${CMAKE_SOURCE_DIR}/exprtk/include)
target_link_libraries(test_plugin_loader_missing_entry PRIVATE Salts::Core)
set_target_properties(
  test_plugin_loader_missing_entry
  PROPERTIES PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)

cmake_add_test(
  TARGET test_ts_plugin_loader
  SOURCES test_ts_plugin_loader.c
  LIBS turbo_script_plugin_loader Salts::TinyTest
  INCLUDES ${CMAKE_SOURCE_DIR}/exprtk/include
  DEPENDS test_plugin_loader_fixture test_plugin_loader_bad_abi
          test_plugin_loader_prefixed_conflict
          test_plugin_loader_wrong_name test_plugin_loader_missing_entry
          test_plugin_loader_init_failure test_plugin_loader_legacy
          test_plugin_loader_cwd_only
  FOLDER turbo_script
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

macro(add_ts_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS turbo_script Salts::TinyTest Salts::CSTL Salts::Coroutine exprtk ta mir_static
    INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
    DEPENDS mapper_tbs fin_tbs ta_tbs ts_tbs net_tbs
    FOLDER turbo_script)
  add_custom_command(
    TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fin_tbs>    $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ta_tbs>     $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ts_tbs>     $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mapper_tbs> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:net_tbs>    $<TARGET_FILE_DIR:${TARGET_NAME}>
    )
endmacro()

# Interpreter test suites (split by module)
add_ts_test(test_turbo_script_basics     test_turbo_script_basics.c)
add_dependencies(test_turbo_script_basics
  test_plugin_loader_fixture test_plugin_loader_prefixed_conflict
  crypto_tbs)
add_ts_test(test_turbo_script_quant      test_turbo_script_quant.c)
add_ts_test(test_turbo_script_scientific test_turbo_script_scientific.c)
add_ts_test(test_turbo_script_io         test_turbo_script_io.c)
add_ts_test(test_turbo_script_timer      test_turbo_script_timer.c)
add_ts_test(test_turbo_script_task       test_turbo_script_task.c)
cmake_add_test(
  TARGET test_turbo_script_host_result
  SOURCES test_turbo_script_host_result.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_registry
  SOURCES test_turbo_script_host_registry.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_module
  SOURCES test_turbo_script_host_module.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_instance
  SOURCES test_turbo_script_host_instance.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_call
  SOURCES test_turbo_script_host_call.c
  LIBS turbo_script_lib Salts::TinyTest exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_state
  SOURCES test_turbo_script_host_state.c
  LIBS turbo_script_lib Salts::TinyTest exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_limits
  SOURCES test_turbo_script_host_limits.c
  LIBS turbo_script_lib Salts::TinyTest exprtk ta mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  SOURCES bench_turbo_script.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk mir_static
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
  FOLDER turbo_script)

# MIR / JIT test suites (split by module)
macro(add_mir_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS turbo_script Salts::TinyTest Salts::CSTL exprtk mir_static
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
