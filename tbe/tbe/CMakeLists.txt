set(TARGET_NAME tbe)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(SCHEMA_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/schema_lexer.re)
set(SCHEMA_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/schema_lexer_gen.c)

add_custom_command(
  OUTPUT ${SCHEMA_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${SCHEMA_LEXER_GEN} ${SCHEMA_LEXER_RE}
  DEPENDS ${SCHEMA_LEXER_RE}
  COMMENT "Generating schema DSL lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(SCHEMA_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/schema_grammar.y)
set(SCHEMA_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.c)
set(SCHEMA_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.h)

add_custom_command(
  OUTPUT ${SCHEMA_GRAMMAR_GEN} ${SCHEMA_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${SCHEMA_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.y
  DEPENDS ${SCHEMA_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating schema DSL parser with lemon"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

add_library(
  ${TARGET_NAME} STATIC
  ${SOURCE_FILES} ${SCHEMA_LEXER_GEN} ${SCHEMA_GRAMMAR_GEN} ${SCHEMA_GRAMMAR_Y}
  ${SCHEMA_LEXER_RE})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils)

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::SchemaBE
  FOLDER tbe/${TARGET_NAME}
  EXPORT_NAME SchemaBE
  NO_INSTALL)

# =============================================================================
# Tests
# =============================================================================

add_executable(test_tbe_parser test/test_tbe_parser.c)
target_link_libraries(test_tbe_parser PRIVATE TurboScript::SchemaBE
                                              TurboNet::TinyTest)
set_target_properties(test_tbe_parser PROPERTIES FOLDER tbe/${TARGET_NAME})
add_test(NAME test_tbe_parser COMMAND test_tbe_parser)

add_executable(test_flags_bdd test/test_flags_bdd.c)
target_link_libraries(test_flags_bdd PRIVATE TurboScript::SchemaBE
                                             TurboNet::TinyTest)
set_target_properties(test_flags_bdd PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_flags_bdd COMMAND test_flags_bdd)

add_executable(test_tbe_parser_bdd test/test_tbe_parser_bdd.c)
target_link_libraries(test_tbe_parser_bdd PRIVATE TurboScript::SchemaBE
                                                  TurboNet::TinyTest)
set_target_properties(test_tbe_parser_bdd PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_tbe_parser_bdd COMMAND test_tbe_parser_bdd)

add_executable(test_benchmark_schemas_bdd test/test_benchmark_schemas_bdd.c)
target_link_libraries(test_benchmark_schemas_bdd PRIVATE TurboScript::SchemaBE
                                                         TurboNet::TinyTest)
set_target_properties(test_benchmark_schemas_bdd PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_benchmark_schemas_bdd COMMAND test_benchmark_schemas_bdd)

add_executable(test_tbe_robustness test/test_tbe_robustness.c)
target_link_libraries(test_tbe_robustness PRIVATE TurboScript::SchemaBE
                                                  TurboNet::TinyTest)
set_target_properties(test_tbe_robustness PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_tbe_robustness COMMAND test_tbe_robustness)

add_executable(verify_fixes test/verify_fixes.c)
target_link_libraries(verify_fixes PRIVATE TurboScript::SchemaBE)
set_target_properties(verify_fixes PROPERTIES FOLDER tbe/tbe)

add_executable(test_optional_and_defaults test/test_optional_and_defaults.c)
target_link_libraries(test_optional_and_defaults PRIVATE TurboScript::SchemaBE
                                                         TurboNet::TinyTest)
set_target_properties(test_optional_and_defaults PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_optional_and_defaults COMMAND test_optional_and_defaults)

add_executable(test_enum_helpers test/test_enum_helpers.c)
target_link_libraries(test_enum_helpers PRIVATE TurboScript::SchemaBE
                                                TurboNet::TinyTest)
set_target_properties(test_enum_helpers PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_enum_helpers COMMAND test_enum_helpers)

# =============================================================================
# 枚举增强功能测试：生成头文件
# =============================================================================

set(C_STRUCT_TEMPLATE ${CMAKE_SOURCE_DIR}/tbe/tbe_compiler/templates/c_structs.mustache)

set(ENUM_ENHANCED_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/test/test_enum_enhanced.schema)
set(ENUM_ENHANCED_HEADER ${CMAKE_BINARY_DIR}/bin/test_enum_enhanced.h)
add_custom_command(
  OUTPUT ${ENUM_ENHANCED_HEADER}
  COMMAND tbe_compiler ${ENUM_ENHANCED_SCHEMA}
          --template ${C_STRUCT_TEMPLATE} --lang c --output ${ENUM_ENHANCED_HEADER}
  WORKING_DIRECTORY $<TARGET_FILE_DIR:tbe_compiler>
  DEPENDS tbe_compiler ${ENUM_ENHANCED_SCHEMA} ${C_STRUCT_TEMPLATE}
  COMMENT "Generating test_enum_enhanced.h"
  VERBATIM)
add_custom_target(generate_test_enum_enhanced_header
                  DEPENDS ${ENUM_ENHANCED_HEADER})

set(ENUM_ENHANCED_SIMPLE_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/test/test_enum_enhanced_simple.schema)
set(ENUM_ENHANCED_SIMPLE_HEADER ${CMAKE_BINARY_DIR}/bin/test_enum_enhanced_simple.h)
add_custom_command(
  OUTPUT ${ENUM_ENHANCED_SIMPLE_HEADER}
  COMMAND tbe_compiler ${ENUM_ENHANCED_SIMPLE_SCHEMA}
          --template ${C_STRUCT_TEMPLATE} --lang c --output ${ENUM_ENHANCED_SIMPLE_HEADER}
  WORKING_DIRECTORY $<TARGET_FILE_DIR:tbe_compiler>
  DEPENDS tbe_compiler ${ENUM_ENHANCED_SIMPLE_SCHEMA} ${C_STRUCT_TEMPLATE}
  COMMENT "Generating test_enum_enhanced_simple.h"
  VERBATIM)
add_custom_target(generate_test_enum_enhanced_simple_header
                  DEPENDS ${ENUM_ENHANCED_SIMPLE_HEADER})

# 功能测试：枚举增强
add_executable(test_enum_enhanced_functional
               test/test_enum_enhanced_functional.c
               ${ENUM_ENHANCED_HEADER})
target_link_libraries(test_enum_enhanced_functional PRIVATE TurboScript::SchemaBE
                                                            TurboNet::TinyTest)
target_include_directories(test_enum_enhanced_functional PRIVATE ${CMAKE_BINARY_DIR}/bin)
add_dependencies(test_enum_enhanced_functional generate_test_enum_enhanced_header)
set_target_properties(test_enum_enhanced_functional PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_enum_enhanced_functional COMMAND test_enum_enhanced_functional)

# 功能测试：枚举增强（简化版）
add_executable(test_enum_enhanced_simple_functional
               test/test_enum_enhanced_simple_functional.c
               ${ENUM_ENHANCED_SIMPLE_HEADER})
target_link_libraries(test_enum_enhanced_simple_functional PRIVATE TurboScript::SchemaBE
                                                                    TurboNet::TinyTest)
target_include_directories(test_enum_enhanced_simple_functional PRIVATE ${CMAKE_BINARY_DIR}/bin)
add_dependencies(test_enum_enhanced_simple_functional generate_test_enum_enhanced_simple_header)
set_target_properties(test_enum_enhanced_simple_functional PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_enum_enhanced_simple_functional COMMAND test_enum_enhanced_simple_functional)

set(UNION_TYPES_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/test/test_union_types.schema)
set(UNION_TYPES_HEADER ${CMAKE_BINARY_DIR}/bin/test_union_types.h)
add_custom_command(
  OUTPUT ${UNION_TYPES_HEADER}
  COMMAND tbe_compiler ${UNION_TYPES_SCHEMA}
          --template ${C_STRUCT_TEMPLATE} --lang c --output ${UNION_TYPES_HEADER}
  WORKING_DIRECTORY $<TARGET_FILE_DIR:tbe_compiler>
  DEPENDS tbe_compiler ${UNION_TYPES_SCHEMA} ${C_STRUCT_TEMPLATE}
  COMMENT "Generating test_union_types.h"
  VERBATIM)
add_custom_target(generate_test_union_types_header
                  DEPENDS ${UNION_TYPES_HEADER})

# 功能测试：联合类型
add_executable(test_union_types_functional
               test/test_union_types_functional.c
               ${UNION_TYPES_HEADER})
target_link_libraries(test_union_types_functional PRIVATE TurboScript::SchemaBE
                                                          TurboNet::TinyTest)
target_include_directories(test_union_types_functional PRIVATE ${CMAKE_BINARY_DIR}/bin)
add_dependencies(test_union_types_functional generate_test_union_types_header)
set_target_properties(test_union_types_functional PROPERTIES FOLDER tbe/tbe)
add_test(NAME test_union_types_functional COMMAND test_union_types_functional)

# =============================================================================
# Benchmarks
# =============================================================================

add_executable(benchmark_tbe_simple test/benchmark_tbe_simple.c)
target_link_libraries(benchmark_tbe_simple PRIVATE TurboScript::SchemaBE
                                                   TurboNet::TinyTest)
set_target_properties(benchmark_tbe_simple PROPERTIES FOLDER tbe/tbe)

add_executable(benchmark_encoding test/benchmark_encoding.c)
target_link_libraries(benchmark_encoding PRIVATE TurboNet::TinyTest)
set_target_properties(benchmark_encoding PROPERTIES FOLDER tbe/tbe)

# =============================================================================
# TBE vs Native Struct Benchmark
# =============================================================================

set(BENCH_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/test/bench.schema)
set(BENCH_HEADER ${CMAKE_CURRENT_BINARY_DIR}/bench_data.h)
set(C_STRUCT_TEMPLATE
    ${CMAKE_SOURCE_DIR}/tbe/tbe_compiler/templates/c_structs.mustache)

add_custom_command(
  OUTPUT ${BENCH_HEADER}
  COMMAND tbe_compiler ${BENCH_SCHEMA} --template ${C_STRUCT_TEMPLATE}
          --lang c --output ${BENCH_HEADER}
  DEPENDS tbe_compiler ${BENCH_SCHEMA} ${C_STRUCT_TEMPLATE}
  COMMENT "Generating benchmark headers with tbe_compiler"
  VERBATIM)
