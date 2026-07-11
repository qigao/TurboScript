add_executable(test_hash test_hash.c)

target_link_libraries(test_hash PRIVATE hash TurboUtils::TinyTest TurboUtils::Core xxHash::xxhash)

target_include_directories(test_hash
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  test_hash PROPERTIES FOLDER modules/hash
                       RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(WIN32)
  add_custom_command(
    TARGET test_hash
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:xxHash::xxhash>
            $<TARGET_FILE_DIR:test_hash>)
endif()

add_test(NAME test_hash COMMAND test_hash)
