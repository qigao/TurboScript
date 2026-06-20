set(TEST_NAME test_exprtk)

add_executable(${TEST_NAME} test_exprtk.c)

target_link_libraries(${TEST_NAME} PRIVATE exprtk TurboNet::TinyTest)

target_include_directories(${TEST_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                         ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# Simple optimization tests
set(TEST_SIMPLE_NAME test_simple_opt)

add_executable(${TEST_SIMPLE_NAME} test_simple_opt.c)

target_link_libraries(${TEST_SIMPLE_NAME} PRIVATE exprtk)

target_include_directories(${TEST_SIMPLE_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_SIMPLE_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                                ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_SIMPLE_NAME} COMMAND ${TEST_SIMPLE_NAME})

# SIMD performance benchmarks
set(TEST_SIMD_NAME test_simd_perf)

add_executable(${TEST_SIMD_NAME} test_simd_perf.c)

target_link_libraries(${TEST_SIMD_NAME} PRIVATE exprtk)
if(UNIX)
  target_link_libraries(${TEST_SIMD_NAME} PRIVATE m)
endif()
target_include_directories(${TEST_SIMD_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_SIMD_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                              ${CMAKE_BINARY_DIR}/bin)

# Optimization tests
set(TEST_OPT_NAME test_exprtk_optimizations)

add_executable(${TEST_OPT_NAME} test_optimizations.c)

target_link_libraries(${TEST_OPT_NAME} PRIVATE exprtk)

target_include_directories(${TEST_OPT_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_OPT_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                             ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_OPT_NAME} COMMAND ${TEST_OPT_NAME})

# SIMD Math Module Tests
set(TEST_MOD_MATH_NAME test_mod_math)

add_executable(${TEST_MOD_MATH_NAME} test_mod_math.c)

target_link_libraries(${TEST_MOD_MATH_NAME} PRIVATE exprtk TurboNet::TinyTest)
if(UNIX)
  target_link_libraries(${TEST_MOD_MATH_NAME} PRIVATE m)
endif()
target_include_directories(${TEST_MOD_MATH_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_MOD_MATH_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                                  ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_MOD_MATH_NAME} COMMAND ${TEST_MOD_MATH_NAME})

# SIMD Helpers Unit Tests
set(TEST_SIMD_HELPERS_NAME test_simd_helpers)

add_executable(${TEST_SIMD_HELPERS_NAME} test_simd_helpers.c)

target_link_libraries(${TEST_SIMD_HELPERS_NAME} PRIVATE exprtk
                                                        TurboNet::TinyTest)

target_include_directories(${TEST_SIMD_HELPERS_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_SIMD_HELPERS_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                                      ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_SIMD_HELPERS_NAME} COMMAND ${TEST_SIMD_HELPERS_NAME})

# IO Module Unit Tests
set(TEST_MOD_IO_NAME test_mod_io)

add_executable(${TEST_MOD_IO_NAME} test_mod_io.c)

target_link_libraries(${TEST_MOD_IO_NAME} PRIVATE exprtk TurboNet::TinyTest)

target_include_directories(${TEST_MOD_IO_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_MOD_IO_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                                ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_MOD_IO_NAME} COMMAND ${TEST_MOD_IO_NAME})

# OOP Runtime Unit Tests
set(TEST_OOP_NAME test_oop)

add_executable(${TEST_OOP_NAME} test_oop.c)

target_link_libraries(${TEST_OOP_NAME} PRIVATE exprtk TurboNet::TinyTest)

target_include_directories(${TEST_OOP_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TEST_OOP_NAME} PROPERTIES FOLDER exprtk/ RUNTIME_OUTPUT_DIRECTORY
                                             ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_OOP_NAME} COMMAND ${TEST_OOP_NAME})
