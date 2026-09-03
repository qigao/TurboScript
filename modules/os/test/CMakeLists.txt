cmake_add_test(
  TARGET test_os_module
  TEST_NAME os_module_tests
  SOURCES test_os.c
  LIBS os exprtk Salts::TinyTest Salts::Core
  FOLDER modules/os
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
