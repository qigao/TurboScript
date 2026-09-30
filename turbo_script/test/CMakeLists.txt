# Plugin loader fixtures use the same output layout as production plugins.
add_library(test_plugin_loader_fixture SHARED plugin_loader_fixture.c)
cmake_config_target(
  test_plugin_loader_fixture
  FOLDER turbo_script
  OUTPUT_NAME loader_fixture
  PLUGIN)
target_include_directories(
  test_plugin_loader_fixture PRIVATE ${CMAKE_SOURCE_DIR}/exprtk/include)
target_link_libraries(test_plugin_loader_fixture PRIVATE Salts::PluginABI)
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
  target_link_libraries(${target} PRIVATE Salts::PluginABI)
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

add_plugin_loader_fixture(test_plugin_loader_function loader_function)
target_compile_definitions(
  test_plugin_loader_function
  PRIVATE FIXTURE_PLUGIN_NAME="loader_function" FIXTURE_FUNCTION_ONLY)

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

add_plugin_loader_fixture(test_plugin_loader_package loader_package)
set_target_properties(
  test_plugin_loader_package
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY
             ${CMAKE_BINARY_DIR}/bin/loader-package/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY
             ${CMAKE_BINARY_DIR}/bin/loader-package/bin/plugins)

add_library(test_plugin_loader_missing_entry SHARED plugin_loader_missing_entry.c)
cmake_config_target(
  test_plugin_loader_missing_entry
  FOLDER turbo_script
  OUTPUT_NAME loader_missing_entry
  PLUGIN)
target_include_directories(
  test_plugin_loader_missing_entry PRIVATE ${CMAKE_SOURCE_DIR}/exprtk/include)
target_link_libraries(test_plugin_loader_missing_entry PRIVATE Salts::PluginABI)
set_target_properties(
  test_plugin_loader_missing_entry
  PROPERTIES PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)

cmake_add_test(
  TARGET test_ts_plugin_loader
  SOURCES test_ts_plugin_loader.c
  LIBS turbo_script_plugin_loader Salts::TinyTest exprtk
  INCLUDES ${CMAKE_SOURCE_DIR}/exprtk/include
  DEPENDS test_plugin_loader_fixture test_plugin_loader_bad_abi
          test_plugin_loader_prefixed_conflict
          test_plugin_loader_wrong_name test_plugin_loader_missing_entry
          test_plugin_loader_init_failure test_plugin_loader_function
          test_plugin_loader_legacy test_plugin_loader_cwd_only
          test_plugin_loader_package
  FOLDER turbo_script
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_tests_properties(
  test_ts_plugin_loader
  PROPERTIES ENVIRONMENT
             "TURBOSCRIPT_ROOT=$<TARGET_FILE_DIR:test_ts_plugin_loader>/loader-package")

macro(add_ts_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS turbo_script Salts::TinyTest Salts::CSTL Salts::Coroutine exprtk ta MIR::MIR
    DEPENDS mapper_tbs fin_tbs ta_tbs ts_tbs net_tbs
    FOLDER turbo_script)
endmacro()

# Generated DataBind Service Plugin qualification is host-only: salts_idl_target
# requires a host salts-idlc when cross-compiling. Core catalog binding still
# compiles on every target platform through turbo_script_plugin_loader.
if(NOT CMAKE_CROSSCOMPILING)
  salts_idl_target(
    TARGET turboscript_databind_service_fixture
    IDL "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service.schema"
    COMPONENT "Image.ImageProcessor"
    VERSION "1.0.0"
    ARTIFACT_NAME "Image.ImageProcessor"
    ARTIFACTS PLUGIN
    SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service_business.c")

  salts_idl_target(
    TARGET turboscript_databind_error_fixture
    IDL "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service_error.schema"
    COMPONENT "ErrorPlugin.StorePlugin"
    VERSION "1.0.0"
    ARTIFACT_NAME "ErrorPlugin.StorePlugin"
    ARTIFACTS PLUGIN
    SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service_error_business.c")

  foreach(_fixture_target IN ITEMS
          ${turboscript_databind_service_fixture_PLUGIN_TARGET}
          ${turboscript_databind_error_fixture_PLUGIN_TARGET})
    set_target_properties(
      ${_fixture_target}
      PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")
  endforeach()
endif()

# Interpreter test suites (split by module)
add_ts_test(test_turbo_script_basics     test_turbo_script_basics.c)
add_dependencies(test_turbo_script_basics
  test_plugin_loader_fixture test_plugin_loader_prefixed_conflict
  test_plugin_loader_function crypto_tbs)
add_ts_test(test_turbo_script_quant      test_turbo_script_quant.c)
add_ts_test(test_turbo_script_scientific test_turbo_script_scientific.c)
add_ts_test(test_turbo_script_io         test_turbo_script_io.c)
add_ts_test(test_turbo_script_timer      test_turbo_script_timer.c)
add_ts_test(test_turbo_script_task       test_turbo_script_task.c)
if(NOT CMAKE_CROSSCOMPILING)
  add_ts_test(
    test_turbo_script_databind_plugin
    test_turbo_script_databind_plugin.c)
  add_dependencies(
    test_turbo_script_databind_plugin
    ${turboscript_databind_service_fixture_PLUGIN_TARGET}
    ${turboscript_databind_error_fixture_PLUGIN_TARGET})
endif()
if(TARGET db_tbs)
  add_ts_test(test_turbo_script_db test_turbo_script_db.c)
  add_dependencies(test_turbo_script_db db_tbs)
  target_compile_definitions(
    test_turbo_script_db
    PRIVATE TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH="${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}")
endif()

cmake_add_test(
  TARGET test_turbo_script_cmeta_bridge
  SOURCES test_turbo_script_cmeta_bridge.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CMeta exprtk
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
  FOLDER turbo_script)
cmake_add_test(
  TARGET test_turbo_script_cflow_lower
  SOURCES test_turbo_script_cflow_lower.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CMeta Salts::CFlow exprtk
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
  FOLDER turbo_script)
cmake_add_test(
  TARGET test_turbo_script_cflow_mir_kernel
  SOURCES test_turbo_script_cflow_mir_kernel.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CMeta Salts::CFlow MIR::MIR exprtk
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)
cmake_add_test(
  TARGET test_turbo_script_cflow_runtime
  SOURCES test_turbo_script_cflow_runtime.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CMeta Salts::CFlow MIR::MIR exprtk
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)
add_dependencies(test_turbo_script_cflow_runtime test_plugin_loader_function)
cmake_add_test(
  TARGET test_turbo_script_cflow_qualification
  SOURCES test_turbo_script_cflow_qualification.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CMeta Salts::CFlow MIR::MIR exprtk
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_benchmark(
  TARGET bench_turbo_script_cflow
  SOURCES bench_turbo_script_cflow.c
  LIBS turbo_script_lib Salts::CMeta Salts::CFlow MIR::MIR exprtk
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)
cmake_add_test(
  TARGET test_turbo_script_host_result
  SOURCES test_turbo_script_host_result.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_registry
  SOURCES test_turbo_script_host_registry.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_module
  SOURCES test_turbo_script_host_module.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_instance
  SOURCES test_turbo_script_host_instance.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_call
  SOURCES test_turbo_script_host_call.c
  LIBS turbo_script_lib Salts::TinyTest exprtk ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_state
  SOURCES test_turbo_script_host_state.c
  LIBS turbo_script_lib Salts::TinyTest exprtk ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  TARGET test_turbo_script_host_limits
  SOURCES test_turbo_script_host_limits.c
  LIBS turbo_script_lib Salts::TinyTest exprtk ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER turbo_script)

cmake_add_test(
  SOURCES bench_turbo_script.c
  LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk MIR::MIR
  FOLDER turbo_script)

# MIR / JIT test suites (split by module)
macro(add_mir_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS turbo_script Salts::TinyTest Salts::CSTL exprtk MIR::MIR
    DEPENDS mapper_tbs
    FOLDER turbo_script)
endmacro()

add_mir_test(test_turbo_script_mir        test_turbo_script_mir.c)
add_mir_test(test_turbo_script_mir_core     test_turbo_script_mir_core.c)
add_mir_test(test_turbo_script_mir_oop      test_turbo_script_mir_oop.c)
add_mir_test(test_turbo_script_mir_advanced test_turbo_script_mir_advanced.c)

# JIT Performance Benchmarks
cmake_add_benchmark(
  SOURCES bench_turbo_script_mir.c
  LIBS turbo_script_lib exprtk MIR::MIR
  FOLDER turbo_script)