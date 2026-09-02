cmake_add_test(
  SOURCES test_fuzzy.c
  LIBS fuzzy Rocida::TinyTest Rocida::Core TurboScript::Tre
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER modules/fuzzy
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
