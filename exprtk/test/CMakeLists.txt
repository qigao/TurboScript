set(EXPRTK_TEST_LIBS exprtk Salts::TinyTest)
set(EXPRTK_TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include)

cmake_add_test(
  TARGET test_exprtk_syntax
  SOURCES test_exprtk_syntax.c
  LIBS TurboScript::ExprTkSyntax Salts::TinyTest
  INCLUDES ${EXPRTK_TEST_INCLUDES}
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
