cmake_add_test(
  SOURCES test_hash.c
  LIBS hash Salts::TinyTest Salts::Core xxHash::xxhash
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER modules/hash
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
