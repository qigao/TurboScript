# Coroutine module for TurboScript
cmake_minimum_required(VERSION 3.15)

set(TARGET_NAME coro)
set(TARGET_NAME_PLUGIN coro_tbs)

set(CORO_SOURCES
    src/minicoro_impl.c
    src/coro_core.c
    src/exprtk_mod_coro.c
)

set(CORO_PLUGIN_SOURCES
    src/coro_plugin.c
)

set(CORO_HEADERS
    include/coro_ctx.h
    src/minicoro.h
)

# ── STATIC lib (for unit tests — direct linking) ─────────────────
add_library(${TARGET_NAME} STATIC ${CORO_SOURCES} ${CORO_HEADERS})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::CORO
  FOLDER modules/coro
  EXPORT_NAME CORO)

target_include_directories(${TARGET_NAME} 
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${TARGET_NAME} 
    PUBLIC exprtk TurboUtils::Core
)

# Set C standard
set_target_properties(${TARGET_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# ── SHARED plugin DLL (loaded at runtime via ts_plugin_load) ─────
add_library(${TARGET_NAME_PLUGIN} SHARED ${CORO_PLUGIN_SOURCES})

cmake_config_target(
  ${TARGET_NAME_PLUGIN}
  FOLDER modules/coro
  OUTPUT_NAME tbs_coro
  PLUGIN)

target_link_libraries(${TARGET_NAME_PLUGIN}
    PRIVATE ${TARGET_NAME}
    PUBLIC exprtk TurboUtils::Core
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
source_group("Source Files" FILES ${CORO_SOURCES})
source_group("Header Files" FILES ${CORO_HEADERS})

# Tests
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# Install headers
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ 
                     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/coro)
