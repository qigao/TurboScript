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
  add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:RulesForge::rules_forge> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TurboUtils::DataBind> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TurboUtils::Parser> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TurboUtils::Core> $<TARGET_FILE_DIR:${TARGET_NAME}>)
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
