set(TARGET_NAME fuzzy)
set(TARGET_NAME_PLUGIN fuzzy_tbs)

set(FUZZY_SOURCES src/exprtk_mod_fuzzy.c)
set(FUZZY_PLUGIN_SOURCES src/fuzzy_plugin.c)
set(FUZZY_HEADERS include/fuzzy.h)

add_library(${TARGET_NAME} STATIC ${FUZZY_SOURCES} ${FUZZY_HEADERS})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  TurboScript::Fuzzy
  FOLDER
  modules/fuzzy
  EXPORT_NAME
  Fuzzy)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk Salts::Core
                                            TurboScript::Tre)

add_library(${TARGET_NAME_PLUGIN} SHARED ${FUZZY_PLUGIN_SOURCES})

cmake_config_target(${TARGET_NAME_PLUGIN} FOLDER modules/fuzzy OUTPUT_NAME
                    fuzzy PLUGIN)

target_link_libraries(${TARGET_NAME_PLUGIN} PRIVATE ${TARGET_NAME})

target_include_directories(${TARGET_NAME_PLUGIN}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fuzzy)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
