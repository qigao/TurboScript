# Parser module for TurboScript
cmake_minimum_required(VERSION 3.15)

set(TARGET_NAME parser)
set(TARGET_NAME_PLUGIN parser_tbs)

set(PARSER_SOURCES
    src/parser_core.c
    src/exprtk_mod_parser.c
)

set(PARSER_PLUGIN_SOURCES
    src/parser_plugin.c
)

set(PARSER_HEADERS
    include/parser_ctx.h
)

# ── STATIC lib (for unit tests) ─────────────────────────────────────────
add_library(${TARGET_NAME} STATIC ${PARSER_SOURCES} ${PARSER_HEADERS})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::Parser
  FOLDER modules/parser
  EXPORT_NAME Parser)

target_include_directories(${TARGET_NAME}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${TARGET_NAME}
    PUBLIC exprtk TurboUtils::Core TurboParser::Parser
)

set_target_properties(${TARGET_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# ── SHARED plugin DLL ───────────────────────────────────────────────────
add_library(${TARGET_NAME_PLUGIN} SHARED ${PARSER_PLUGIN_SOURCES})

cmake_config_target(
  ${TARGET_NAME_PLUGIN}
  FOLDER modules/parser
  OUTPUT_NAME parser
  PLUGIN)

target_link_libraries(${TARGET_NAME_PLUGIN}
    PRIVATE ${TARGET_NAME}
    PUBLIC exprtk TurboUtils::Core TurboParser::Parser
)

target_include_directories(${TARGET_NAME_PLUGIN}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(${TARGET_NAME_PLUGIN}
    PROPERTIES PREFIX ""
               VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Source grouping for IDEs
source_group("Source Files" FILES ${PARSER_SOURCES})
source_group("Header Files" FILES ${PARSER_HEADERS})

# Tests
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# Install headers
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/parser)
