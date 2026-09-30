# Generic database module backed only by TurboDB::Orm.
# Driver choice is entirely user-configured at runtime; no native database
# client is linked here and no driver is loaded implicitly.
set(TARGET_NAME db_tbs)

add_library(
  ${TARGET_NAME} SHARED
  src/db_plugin.c
  ${CMAKE_SOURCE_DIR}/turbo_script/src/databind/turbo_script_class_databind.c
  ${CMAKE_SOURCE_DIR}/turbo_script/src/databind/turbo_script_class_databind.h)

cmake_config_target(
  ${TARGET_NAME}
  FOLDER modules/db
  OUTPUT_NAME db
  PLUGIN)

target_link_libraries(
  ${TARGET_NAME}
  PRIVATE exprtk Orm::C Salts::DataBind)

target_include_directories(
  ${TARGET_NAME}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_SOURCE_DIR}/turbo_script/src/databind)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

if(BUILD_TESTING)
  cmake_add_test(
    TARGET test_turbo_script_db
    SOURCES ${CMAKE_SOURCE_DIR}/turbo_script/test/test_turbo_script_db.c
    LIBS turbo_script Salts::TinyTest Salts::CSTL Salts::Coroutine exprtk MIR::MIR
    DEPENDS db_tbs
    DEFS TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH="${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}"
    FOLDER modules/db)
endif()
