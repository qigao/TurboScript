project(turbo_script)

set(TARGET_NAME turbo_script)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

# Collect sources
cmake_collect_files(SOURCE_FILES RECURSE)

# ── STATIC lib for testing ────────────────────────────────────────
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)

target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE exprtk mir_static
  PUBLIC TurboNet::Utils TurboNet::Parser)

# ── SHARED library ────────────────────────────────────────────────
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

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
  PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)

target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

target_link_libraries(
  ${TARGET_NAME}
  PRIVATE mir_static
  PUBLIC TurboNet::Utils exprtk)

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)
 

if(BUILD_TESTS AND ENABLE_TESTS)
  add_subdirectory(test)
endif()
add_subdirectory(app)
