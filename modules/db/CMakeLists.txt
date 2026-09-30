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
