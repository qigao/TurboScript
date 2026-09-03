project(turbo_script)

set(TARGET_NAME turbo_script)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)
set(TARGET_NAME_PLUGIN_LOADER ${TARGET_NAME}_plugin_loader)

# Host-only native module loader. It is deliberately separate from the JIT
# runtime so the ABI boundary can be tested without admitting task/timer code.
add_library(${TARGET_NAME_PLUGIN_LOADER} STATIC src/host/ts_plugin_loader.c)
cmake_config_target(${TARGET_NAME_PLUGIN_LOADER} FOLDER turbo_script)
target_include_directories(
  ${TARGET_NAME_PLUGIN_LOADER}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_SOURCE_DIR}/exprtk/include)
target_link_libraries(${TARGET_NAME_PLUGIN_LOADER} PRIVATE Salts::Core)

# Source files listed explicitly to avoid CMake globbing configuration issues
set(SOURCE_FILES
  src/host/turbo_script_host_instance.c
  src/host/turbo_script_host_limits.c
  src/host/turbo_script_host_module.c
  src/host/turbo_script_host_registry.c
  src/host/turbo_script_host_result.c
  src/mir_mod.c
  src/turbo_script.c
  src/turbo_script_closure_analysis.c
  src/turbo_script_closure_analysis.h
  src/turbo_script_internal.h
  src/turbo_script_registry.c
  src/turbo_script_task.c
  src/turbo_script_task.h
  src/turbo_script_timer.c
  src/turbo_script_timer.h
)

# MIR backend modules. turbo_script_mir.c still owns the main AST lowering path
# while cohesive subsystems are moved into src/mir/*.c modules.
set(MIR_BACKEND_FILES
  src/turbo_script_mir.c
  src/mir/turbo_script_mir_api.c
  src/mir/turbo_script_mir_cache.c
  src/mir/turbo_script_mir_compiler.c
  src/mir/turbo_script_mir_constant.c
  src/mir/turbo_script_mir_data.c
  src/mir/turbo_script_mir_functions.c
  src/mir/turbo_script_mir_oop.c
  src/mir/turbo_script_mir_runtime.c
  src/mir/turbo_script_mir_value_emit.c
  src/mir/turbo_script_mir_value_runtime.c
)

# Combine all sources
set(ALL_SOURCE_FILES ${SOURCE_FILES} ${MIR_BACKEND_FILES})

# ── STATIC lib for testing ────────────────────────────────────────
add_library(${TARGET_NAME_LIB} STATIC ${ALL_SOURCE_FILES})
cmake_config_target(${TARGET_NAME_LIB} FOLDER turbo_script)

target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir
          ${CMAKE_SOURCE_DIR}/exprtk/src
          ${CMAKE_CURRENT_SOURCE_DIR}/src/host
          ${CMAKE_CURRENT_SOURCE_DIR}/src/mir)  # MIR backend internal headers

target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE exprtk mir_static Salts::CSTL ${TARGET_NAME_PLUGIN_LOADER}
  PUBLIC Salts::Core Salts::DataBind
  PRIVATE Salts::Cron Salts::Coroutine)

# Enable strict warnings for modularized code
if(MSVC)
  target_compile_options(${TARGET_NAME_LIB} PRIVATE /W4)
else()
  target_compile_options(${TARGET_NAME_LIB} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── SHARED library ────────────────────────────────────────────────
add_library(${TARGET_NAME} SHARED ${ALL_SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  TurboScript::TurboScript
  FOLDER
  turbo_script
  EXPORT_NAME
  TurboScript)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir
          ${CMAKE_SOURCE_DIR}/exprtk/src
          ${CMAKE_CURRENT_SOURCE_DIR}/src/host
          ${CMAKE_CURRENT_SOURCE_DIR}/src/mir)  # MIR backend internal headers

target_compile_definitions(
  ${TARGET_NAME}
  PRIVATE TURBO_SCRIPT_BUILD_SHARED
  INTERFACE TURBO_SCRIPT_USE_SHARED)

target_link_libraries(
  ${TARGET_NAME}
  PRIVATE exprtk mir_static Salts::CSTL ${TARGET_NAME_PLUGIN_LOADER}
  PUBLIC Salts::Core
  PRIVATE Salts::Cron Salts::Coroutine)

if(WIN32)
  add_custom_command(
    TARGET ${TARGET_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Salts::Cron> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Salts::Mustache> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMENT "Synchronizing TurboScript runtime dependencies"
    VERBATIM)
endif()

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)
 

if(BUILD_TESTS AND ENABLE_TESTS)
  add_subdirectory(test)
endif()
add_subdirectory(app)
