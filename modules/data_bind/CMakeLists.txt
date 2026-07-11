# tbs_data_bind plugin output — import("data_bind")
#
# Provides binary-to-script-object parsing using JIT-compiled TBE schemas.
# Depends on:TurboUtils::DataBind (the tbe/data_bind library) and exprtk.

set(TARGET_NAME data_bind_tbs)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

file(GLOB_RECURSE SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)

# ── Static lib (used by tests) ──────────────────────────────────────────────
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME_LIB}
  FOLDER modules/data_bind
  NO_INSTALL)

target_include_directories(${TARGET_NAME_LIB}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(${TARGET_NAME_LIB}
  PRIVATE
    exprtk
   TurboUtils::DataBind)

# ── Shared plugin DLL ────────────────────────────────────────────────────────
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  FOLDER modules/data_bind
  PLUGIN)

target_include_directories(${TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(${TARGET_NAME}
  PRIVATE
    exprtk
   TurboUtils::DataBind)

set_target_properties(${TARGET_NAME}
  PROPERTIES
    FOLDER   modules/data_bind
    PREFIX   ""
    VERSION  ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTS AND ENABLE_TESTS)
  add_subdirectory(tests)
endif()
