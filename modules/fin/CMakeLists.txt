# fin_plugin — import("fin")
set(TARGET_NAME fin)
set(TARGET_NAME_PLUGIN fin_tbs)

cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*")

# ── STATIC lib (for unit tests — direct linking) ─────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME} FOLDER modules/fin EXPORT_NAME Fin)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fin>)
target_include_directories(${TARGET_NAME} PRIVATE ${SIMDE_INCLUDE_DIRS})

target_link_libraries(${TARGET_NAME} PUBLIC exprtk Salts::Core)
if(UNIX)
  target_link_libraries(${TARGET_NAME} PRIVATE m)
endif()
# ── SHARED plugin DLL (loaded at runtime via ts_plugin_load) ─────
add_library(${TARGET_NAME_PLUGIN} SHARED ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME_PLUGIN} FOLDER modules/fin OUTPUT_NAME
                    fin PLUGIN)

target_link_libraries(${TARGET_NAME_PLUGIN} PRIVATE ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME_PLUGIN} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                ${SIMDE_INCLUDE_DIRS})

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fin)

# ── Tests ─────────────────────────────────────────────────────────
add_subdirectory(test)
