set(TARGET_NAME hash)
set(TARGET_NAME_PLUGIN hash_tbs)

set(HASH_SOURCES src/exprtk_mod_hash.c)
set(HASH_PLUGIN_SOURCES src/hash_plugin.c)
set(HASH_HEADERS include/hash.h)

add_library(${TARGET_NAME} STATIC ${HASH_SOURCES} ${HASH_HEADERS})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::Hash
  FOLDER modules/hash
  EXPORT_NAME Hash)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                         $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk xxHash::xxhash Salts::Core)

add_library(${TARGET_NAME_PLUGIN} SHARED ${HASH_PLUGIN_SOURCES})

cmake_config_target(
  ${TARGET_NAME_PLUGIN}
  FOLDER modules/hash
  OUTPUT_NAME hash
  PLUGIN)

target_link_libraries(${TARGET_NAME_PLUGIN} PRIVATE ${TARGET_NAME})

target_include_directories(${TARGET_NAME_PLUGIN}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hash)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
