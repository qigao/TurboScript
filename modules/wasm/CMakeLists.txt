# wasm plugin output — import("wasm")
set(TARGET_NAME wasm_tbs)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*")

# Static lib for tests/integration
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})
cmake_config_target(${TARGET_NAME_LIB} FOLDER modules/wasm)
target_link_libraries(${TARGET_NAME_LIB} PUBLIC exprtk TurboScript::Wasm
                                                TurboNet::CoroNet)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
cmake_config_target(
  ${TARGET_NAME}
  FOLDER modules/wasm
  OUTPUT_NAME wasm
  PLUGIN)
target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboScript::Wasm
                                            TurboNet::CoroNet)
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
