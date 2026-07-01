# sqlite_tbs — import("sqlite")
set(TARGET_NAME sqlite_tbs)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# Find SQLite3
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Static lib for tests
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME_LIB}
  FOLDER modules/sqlite
  NO_INSTALL)

target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE exprtk   unofficial::sqlite3::sqlite3
  PUBLIC TurboNet::CoroNet)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sqlite>)

# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  FOLDER modules/sqlite
  PLUGIN)

target_link_libraries(
  ${TARGET_NAME}
  PRIVATE   exprtk unofficial::sqlite3::sqlite3
  PUBLIC TurboNet::CoroNet)
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER modules/sqlite
             PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sqlite)

if(BUILD_TESTS AND ENABLE_TESTS)
  add_subdirectory(tests)
endif()
