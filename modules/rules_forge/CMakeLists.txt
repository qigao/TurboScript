set(TARGET_NAME rules_forge_tbs)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

find_package(RulesForge CONFIG REQUIRED PATHS "${RulesForge_ROOT}")

file(GLOB_RECURSE SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)

add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME_LIB}
  FOLDER modules/rules_forge
  NO_INSTALL)

target_include_directories(${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(${TARGET_NAME_LIB}
  PRIVATE exprtk RulesForge::rule_forge)

add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  FOLDER modules/rules_forge
  PLUGIN)

target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(${TARGET_NAME}
  PRIVATE exprtk RulesForge::rule_forge)

set_target_properties(${TARGET_NAME}
  PROPERTIES
    FOLDER modules/rules_forge
    PREFIX ""
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
