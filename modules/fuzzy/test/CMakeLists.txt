add_executable(test_fuzzy test_fuzzy.c)

target_link_libraries(test_fuzzy PRIVATE fuzzy TurboUtils::TinyTest
                                         TurboUtils::Core TurboScript::Tre)

target_include_directories(test_fuzzy
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  test_fuzzy PROPERTIES FOLDER modules/fuzzy
                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME test_fuzzy COMMAND test_fuzzy)
