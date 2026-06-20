# =============================================================================
# Executable
# =============================================================================

add_executable(tbe_compiler main.c compiler_core.c mustache_helpers.c)
target_link_libraries(tbe_compiler PRIVATE TurboScript::SchemaBE
                                           TurboScript::DataBind
                                           TurboNet::Parser TurboNet::Mustache)
target_include_directories(tbe_compiler
                           PRIVATE ${PROJECT_SOURCE_DIR}/shared/utils/include)
set_target_properties(tbe_compiler PROPERTIES FOLDER tbe/tbe_compiler)

# =============================================================================
# Copy templates next to executable for easy access
# =============================================================================

add_custom_command(
  TARGET tbe_compiler
  POST_BUILD
  COMMAND
    ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/templates
    $<TARGET_FILE_DIR:tbe_compiler>/templates
  COMMENT "Copying template files to output directory")

# =============================================================================
# Convenience target
# =============================================================================

add_custom_target(
  generate_tbe_compiler
  COMMAND
    "$<TARGET_FILE:tbe_compiler>"
    "${CMAKE_CURRENT_SOURCE_DIR}/example.schema" --lang c --output
    "${CMAKE_CURRENT_BINARY_DIR}/output.h"
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:tbe_compiler>"
  COMMENT "Generating TBE definitions from schema")

# =============================================================================
# Tests
# =============================================================================

add_executable(test_tbe_compiler test_tbe_compiler.c compiler_core.c mustache_helpers.c)
target_link_libraries(test_tbe_compiler PRIVATE TurboScript::SchemaBE
                                                TurboScript::DataBind
                                                TurboNet::TinyTest TurboNet::Mustache)
target_compile_definitions(
  test_tbe_compiler
  PRIVATE
    SCHEMA_EXAMPLE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/example.schema"
    C_STRUCT_TEMPLATE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/templates/c_structs.mustache"
)
set_target_properties(test_tbe_compiler PROPERTIES FOLDER tbe/tbe_compiler)
