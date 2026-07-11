# Coroutine module tests
cmake_minimum_required(VERSION 3.15)

add_executable(test_coro test_coro.c)

target_link_libraries(
  test_coro
  PRIVATE coro
  PRIVATE exprtk
  PRIVATE TurboUtils::TinyTest
  PRIVATE TurboUtils::Core)

# Enable testing
add_test(NAME coro_unit_tests COMMAND test_coro)

# Output to bin directory
set_target_properties(test_coro PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                           ${CMAKE_BINARY_DIR}/bin)
cmake_config_target(test_coro FOLDER modules/coro PLUGIN)
