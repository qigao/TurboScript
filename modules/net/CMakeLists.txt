set(TARGET_NAME net_tbs)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*")

# Static lib for tests
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME_LIB} FOLDER modules/net)

target_link_libraries(${TARGET_NAME_LIB} PUBLIC exprtk TurboHttp::HttpClient turbo_script)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/net>)
# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME} FOLDER modules/net OUTPUT_NAME tbs_net
                    PLUGIN)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboHttp::HttpClient turbo_script)
add_custom_command(
  TARGET ${TARGET_NAME}
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:TurboHttp::HttpClient> $<TARGET_FILE_DIR:${TARGET_NAME}>)
target_include_directories(${TARGET_NAME} PRIVATE ${net_INCLUDES})
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/net)
add_subdirectory(test)
