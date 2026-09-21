set(TARGET_NAME exprtk)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(exprtk_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/exprtk_lexer.re)
set(exprtk_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/exprtk_lexer_gen.c)

add_custom_command(
  OUTPUT ${exprtk_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${exprtk_LEXER_GEN} ${exprtk_LEXER_RE}
  DEPENDS ${exprtk_LEXER_RE}
  COMMENT "Generating exprtk lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(exprtk_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/exprtk_grammar.y)
set(exprtk_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.c)
set(exprtk_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.h)

add_custom_command(
  OUTPUT ${exprtk_GRAMMAR_GEN} ${exprtk_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${exprtk_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/exprtk_grammar_gen.y
  DEPENDS ${exprtk_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating exprtk parser with lemon"
  VERBATIM)

# =============================================================================
# Static frontend/runtime embedded by TurboScript and plugin binaries
# =============================================================================
cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*")
set(EXPRTK_SYNTAX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/grammar/exprtk_parser.c)
list(REMOVE_ITEM SOURCE_FILES ${EXPRTK_SYNTAX_SOURCE})

add_library(
  exprtk_syntax STATIC
  ${EXPRTK_SYNTAX_SOURCE}
  ${exprtk_LEXER_GEN}
  ${exprtk_GRAMMAR_GEN}
  ${exprtk_LEXER_RE}
  ${exprtk_GRAMMAR_Y}
  ${exprtk_GRAMMAR_H})

cmake_config_target(
  exprtk_syntax
  ALIAS
  TurboScript::ExprTkSyntax
  FOLDER
  "exprtk"
  EXPORT_NAME
  ExprTkSyntax)

target_include_directories(
  exprtk_syntax
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(exprtk_syntax PUBLIC Salts::Core Salts::DataBind Salts::DateTimeParser)

add_library(
  ${TARGET_NAME} STATIC
  ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  TurboScript::ExprTk
  FOLDER
  "exprtk"
  EXPORT_NAME
  ExprTk)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_include_directories(${TARGET_NAME} PRIVATE ${SIMDE_INCLUDE_DIRS})

target_link_libraries(
  ${TARGET_NAME}
  PUBLIC TurboScript::ExprTkSyntax Salts::Core Salts::DataBind Salts::DateTimeParser
  PRIVATE Salts::CSTL Salts::Mustache miniblas_linalg)

if(NOT MSVC)
  target_link_libraries(${TARGET_NAME} PUBLIC m)
endif()

# Enable AVX/AVX2 for SIMD optimizations
if(MSVC)
  target_compile_options(${TARGET_NAME} PRIVATE /arch:AVX2)
else()
  target_compile_options(${TARGET_NAME} PRIVATE -mavx2 -mfma)
endif()

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Install headers
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)
install(FILES ${exprtk_GRAMMAR_H} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(BUILD_TESTS AND ENABLE_TESTS)
  add_subdirectory(test)
endif()
