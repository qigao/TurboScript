cmake_add_test(
  SOURCES test_hash.c
  LIBS hash Rocida::TinyTest Rocida::Core xxHash::xxhash
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER modules/hash
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(WIN32)
  add_custom_command(
    TARGET test_hash
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:xxHash::xxhash>
            $<TARGET_FILE_DIR:test_hash>)
endif()
