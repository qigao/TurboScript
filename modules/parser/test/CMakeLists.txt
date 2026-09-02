# Parser module tests
cmake_add_test(
  TARGET test_parser
  TEST_NAME parser_module
  SOURCES test_parser.c
  LIBS parser exprtk Rocida::Core TurboParser::Parser Rocida::TinyTest
  FOLDER modules/parser/tests)
