# tbs_ts plugin output — import("ts")
set(TARGET_NAME ts)
set(TARGET_NAME_PLUGIN ts_tbs)

cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*")

# ── STATIC lib (for unit tests — direct linking) ─────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  TurboScript::Ts
  FOLDER
  modules/ts
  EXPORT_NAME
  Ts)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)
target_include_directories(${TARGET_NAME} PRIVATE ${SIMDE_INCLUDE_DIRS})

target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboUtils::Core)
if(UNIX)
  target_link_libraries(${TARGET_NAME} PRIVATE m)
endif()
# ── SHARED plugin DLL (loaded at runtime via ts_plugin_load) ─────
add_library(${TARGET_NAME_PLUGIN} SHARED ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME_PLUGIN} FOLDER modules/ts OUTPUT_NAME tbs_ts PLUGIN
)# Plugins are runtime-loaded, not exported for linking

target_link_libraries(${TARGET_NAME_PLUGIN} PUBLIC ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME_PLUGIN} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                ${SIMDE_INCLUDE_DIRS})

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# Install headers
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ts)

add_subdirectory(test)
