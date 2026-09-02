# Coroutine module tests
cmake_minimum_required(VERSION 3.15)

cmake_add_test(
  TARGET test_coro
  TEST_NAME coro_unit_tests
  SOURCES test_coro.c
  LIBS coro exprtk Rocida::TinyTest Rocida::Core
  FOLDER modules/coro
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
