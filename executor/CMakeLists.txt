set(TARGET_NAME executor)

file(GLOB EXECUTOR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(${TARGET_NAME} STATIC ${EXECUTOR_SOURCES})

target_include_directories(${TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${CMAKE_SOURCE_DIR}/tScript/fin/include
    ${CMAKE_SOURCE_DIR}/tScript/exprtk/include
)

target_link_libraries(${TARGET_NAME} PRIVATE TurboNet::Utils)

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::Executor
  FOLDER turbo_script/executor
  EXPORT_NAME Executor
  NO_INSTALL)
