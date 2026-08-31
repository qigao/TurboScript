cmake_minimum_required(VERSION 3.15)

set(TARGET_NAME os)
set(TARGET_NAME_PLUGIN os_tbs)

set(OS_SOURCES src/exprtk_mod_os.c)
set(OS_PLUGIN_SOURCES src/os_plugin.c)
set(OS_HEADERS include/os.h)

add_library(${TARGET_NAME} STATIC ${OS_SOURCES} ${OS_HEADERS})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::OS
  FOLDER modules/os
  EXPORT_NAME OS)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboUtils::Core TurboParser::Cron)

add_library(${TARGET_NAME_PLUGIN} SHARED ${OS_PLUGIN_SOURCES})

cmake_config_target(
  ${TARGET_NAME_PLUGIN}
  FOLDER modules/os
  OUTPUT_NAME os
  PLUGIN)

target_link_libraries(${TARGET_NAME_PLUGIN} PRIVATE ${TARGET_NAME} exprtk TurboUtils::Core TurboParser::Cron)
target_include_directories(${TARGET_NAME_PLUGIN} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/os)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
