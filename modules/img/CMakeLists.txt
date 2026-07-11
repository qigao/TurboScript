set(TARGET_NAME img)
set(TARGET_NAME_PLUGIN img_tbs)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# ── STATIC lib (for unit tests — direct linking) ─────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::IMG
  FOLDER modules/img
  EXPORT_NAME IMG
  NO_INSTALL)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                         $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboUtils::Core)

find_path(STB_INCLUDE_DIRS "stb_image.h")
target_include_directories(${TARGET_NAME} PRIVATE ${STB_INCLUDE_DIRS})

if(UNIX)
  target_link_libraries(${TARGET_NAME} PRIVATE m)
endif()

# ── SHARED plugin DLL (loaded at runtime via ts_plugin_load) ─────
add_library(${TARGET_NAME_PLUGIN} SHARED ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME_PLUGIN}
  FOLDER modules/img
  PLUGIN) # Plugins are runtime-loaded, not exported for linking

target_link_libraries(
  ${TARGET_NAME_PLUGIN}
  PRIVATE ${TARGET_NAME}
  PUBLIC exprtk TurboUtils::Core)

target_include_directories(${TARGET_NAME_PLUGIN}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                   ${STB_INCLUDE_DIRS})

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# Install headers
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/img)

add_subdirectory(test)
