set(TARGET_NAME crypto)
set(TARGET_NAME_PLUGIN crypto_plugin)

set(CRYPTO_SOURCES src/exprtk_mod_crypto.c)
set(CRYPTO_PLUGIN_SOURCES src/crypto_plugin.c)
set(CRYPTO_HEADERS include/crypto.h)

add_library(${TARGET_NAME} STATIC ${CRYPTO_SOURCES} ${CRYPTO_HEADERS})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::Crypto
  FOLDER modules/crypto
  EXPORT_NAME Crypto
  NO_INSTALL)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                         $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk monocypher TurboNet::Utils)

add_library(${TARGET_NAME_PLUGIN} SHARED ${CRYPTO_PLUGIN_SOURCES})

cmake_config_target(
  ${TARGET_NAME_PLUGIN}
  FOLDER modules/crypto
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
                     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/crypto)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
