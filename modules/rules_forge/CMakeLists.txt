set(TARGET_NAME rules_forge_tbs)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

find_package(RulesForge CONFIG REQUIRED PATHS "${RulesForge_ROOT}")

cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c" "*.h")

add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME_LIB}
  FOLDER modules/rules_forge)

target_include_directories(${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(${TARGET_NAME_LIB}
  PRIVATE exprtk RulesForge::rules_forge)

add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  FOLDER modules/rules_forge
  OUTPUT_NAME tbs_rules_forge
  PLUGIN)

target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(${TARGET_NAME}
  PRIVATE exprtk RulesForge::rules_forge)

if(WIN32)
  add_custom_target(${TARGET_NAME}_runtime_deps
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:RulesForge::rules_forge> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TurboParser::Parser> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TurboUtils::Core> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TurboParser::DataBind> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMENT "Synchronizing RulesForge runtime dependencies"
    VERBATIM)
  add_dependencies(${TARGET_NAME} ${TARGET_NAME}_runtime_deps)
endif()

set_target_properties(${TARGET_NAME}
  PROPERTIES
    FOLDER modules/rules_forge
    PREFIX ""
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
