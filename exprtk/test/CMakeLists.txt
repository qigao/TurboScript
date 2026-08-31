set(EXPRTK_TEST_LIBS exprtk TurboUtils::TinyTest)
set(EXPRTK_TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(test_plugin_loader_fixture SHARED plugin_loader_fixture.c)
cmake_config_target(
  test_plugin_loader_fixture
  FOLDER exprtk
  OUTPUT_NAME loader_fixture
  PLUGIN)
target_include_directories(
  test_plugin_loader_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_plugin_loader_fixture PRIVATE TurboUtils::Core)
set_target_properties(
  test_plugin_loader_fixture
  PROPERTIES PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)

function(add_plugin_loader_fixture target output_name)
  add_library(${target} SHARED plugin_loader_fixture.c)
  cmake_config_target(
    ${target}
    FOLDER exprtk
    OUTPUT_NAME ${output_name}
    PLUGIN)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  target_link_libraries(${target} PRIVATE TurboUtils::Core)
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
  FOLDER exprtk
  OUTPUT_NAME loader_missing_entry
  PLUGIN)
target_include_directories(
  test_plugin_loader_missing_entry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_plugin_loader_missing_entry PRIVATE TurboUtils::Core)
set_target_properties(
  test_plugin_loader_missing_entry
  PROPERTIES PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)

cmake_add_test(
  TARGET test_ts_plugin_loader
  SOURCES test_ts_plugin_loader.c
  LIBS ${EXPRTK_TEST_LIBS}
  INCLUDES ${EXPRTK_TEST_INCLUDES}
  DEPENDS test_plugin_loader_fixture test_plugin_loader_bad_abi
          test_plugin_loader_prefixed_conflict
          test_plugin_loader_wrong_name test_plugin_loader_missing_entry
          test_plugin_loader_init_failure test_plugin_loader_legacy
          test_plugin_loader_cwd_only
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  SOURCES test_exprtk.c
  LIBS ${EXPRTK_TEST_LIBS}
  INCLUDES ${EXPRTK_TEST_INCLUDES}
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  SOURCES test_exprtk_checked_registration.c
  LIBS ${EXPRTK_TEST_LIBS}
  INCLUDES ${EXPRTK_TEST_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  SOURCES test_simple_opt.c
  LIBS exprtk
  INCLUDES ${EXPRTK_TEST_INCLUDES}
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  TARGET test_exprtk_optimizations
  SOURCES test_optimizations.c
  LIBS exprtk
  INCLUDES ${EXPRTK_TEST_INCLUDES}
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_benchmark(
  SOURCES test_simd_perf.c
  LIBS exprtk
  INCLUDES ${EXPRTK_TEST_INCLUDES}
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  SOURCES test_mod_math.c
  LIBS ${EXPRTK_TEST_LIBS}
  INCLUDES ${EXPRTK_TEST_INCLUDES}
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(UNIX)
  target_link_libraries(test_simd_perf PRIVATE m)
  target_link_libraries(test_mod_math PRIVATE m)
endif()

cmake_add_test(
  SOURCES test_simd_helpers.c test_mod_io.c test_oop.c
  LIBS ${EXPRTK_TEST_LIBS}
  INCLUDES ${EXPRTK_TEST_INCLUDES}
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  TARGET test_tiny_regex
  SOURCES test_re.c
  LIBS ${EXPRTK_TEST_LIBS}
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER exprtk
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
