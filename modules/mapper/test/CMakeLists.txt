cmake_add_test(
  TARGET test_mapper_module
  TEST_NAME mapper_module_tests
  SOURCES test_mapper.c
  LIBS mapper exprtk Rocida::TinyTest Rocida::Core TurboParser::Parser
  FOLDER modules/mapper
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
