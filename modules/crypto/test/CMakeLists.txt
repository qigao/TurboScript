add_executable(test_crypto test_crypto.c)

target_link_libraries(test_crypto PRIVATE crypto TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(test_crypto
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  test_crypto PROPERTIES FOLDER modules/crypto
                         RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME test_crypto COMMAND test_crypto)
