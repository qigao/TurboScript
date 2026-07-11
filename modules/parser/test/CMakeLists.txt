# Parser module tests
add_executable(test_parser test_parser.c)

cmake_config_target(test_parser FOLDER modules/parser/tests NO_INSTALL)

target_link_libraries(test_parser PRIVATE 
    parser
    exprtk 
    TurboUtils::Core
    TurboUtils::Parser
    TurboUtils::TinyTest
)

add_test(NAME parser_module COMMAND test_parser)
