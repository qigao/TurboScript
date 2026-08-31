cmake_minimum_required(VERSION 3.15)

set(TARGET_NAME mapper)
set(TARGET_NAME_PLUGIN mapper_tbs)

set(MAPPER_SOURCES src/exprtk_mod_mapper.c)
set(MAPPER_PLUGIN_SOURCES src/mapper_plugin.c)
set(MAPPER_HEADERS include/mapper.h)

add_library(${TARGET_NAME} STATIC ${MAPPER_SOURCES} ${MAPPER_HEADERS})
cmake_config_target(${TARGET_NAME}
  ALIAS TurboScript::Mapper
  FOLDER modules/mapper
  EXPORT_NAME Mapper)
target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboUtils::Core TurboParser::Parser)

add_library(${TARGET_NAME_PLUGIN} SHARED ${MAPPER_PLUGIN_SOURCES})
cmake_config_target(${TARGET_NAME_PLUGIN}
  FOLDER modules/mapper
  OUTPUT_NAME mapper
  PLUGIN)
target_include_directories(${TARGET_NAME_PLUGIN}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${TARGET_NAME_PLUGIN}
  PRIVATE ${TARGET_NAME}
  PUBLIC exprtk TurboUtils::Core TurboParser::Parser)
set_target_properties(${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX "" VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mapper)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
