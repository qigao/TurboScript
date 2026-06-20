add_executable(test_regex_module test_regex.c)

target_link_libraries(test_regex_module 
    PRIVATE regex exprtk libfsm::libre TurboNet::TinyTest
)

target_include_directories(test_regex_module
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_regex_module COMMAND test_regex_module)

set_target_properties(test_regex_module 
    PROPERTIES FOLDER modules/regex
)
