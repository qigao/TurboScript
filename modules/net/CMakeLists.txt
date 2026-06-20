set(TARGET_NAME net_plugin)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# Static lib for tests
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME_LIB}
  FOLDER modules/net
  NO_INSTALL)

target_link_libraries(${TARGET_NAME_LIB} PUBLIC exprtk TurboHttp::HttpClient)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/net>)
# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  FOLDER modules/net
  PLUGIN)

target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboHttp::HttpClient)
target_include_directories(${TARGET_NAME} PRIVATE ${net_INCLUDES})
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER modules/net
             PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME "net")
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/net)
add_subdirectory(test)
