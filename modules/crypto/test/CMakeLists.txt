cmake_add_test(
  SOURCES test_crypto.c
  LIBS crypto TurboUtils::TinyTest TurboUtils::Core
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER modules/crypto
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
