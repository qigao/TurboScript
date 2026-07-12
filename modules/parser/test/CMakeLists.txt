# Parser module tests
cmake_add_test(
  TARGET test_parser
  TEST_NAME parser_module
  SOURCES test_parser.c
  LIBS parser exprtk TurboUtils::Core TurboUtils::Parser TurboUtils::TinyTest
  FOLDER modules/parser/tests)
