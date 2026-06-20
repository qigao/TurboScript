# wasm_plugin — import("wasm")
set(TARGET_NAME wasm_plugin)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

# Static lib for tests/integration
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})
target_link_libraries(${TARGET_NAME_LIB} PUBLIC exprtk TurboScript::Wasm
                                                TurboNet::CoroNet)
set_target_properties(${TARGET_NAME_LIB} PROPERTIES FOLDER
                                                    modules/wasm)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
target_link_libraries(${TARGET_NAME} PUBLIC exprtk TurboScript::Wasm
                                            TurboNet::CoroNet)
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER modules/wasm
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
