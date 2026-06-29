# =============================================================================
# Library
# =============================================================================

set(TARGET_NAME data_bind)

add_library(${TARGET_NAME} SHARED data_bind.c data_bind.h tbe_helpers.c)



target_compile_definitions(${TARGET_NAME} PRIVATE DATA_BIND_BUILD_DLL)
if(WIN32)
  target_compile_definitions(${TARGET_NAME}
    INTERFACE DATA_BIND_USE_DLL)
endif()

target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
         $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tbe/tbe/include
    ${CMAKE_SOURCE_DIR}/vendor/mir
)

target_link_libraries(${TARGET_NAME}
  PRIVATE
    TurboScript::SchemaBE
    mir_static
    libfsm::libre
    libfsm::libfsm
  PUBLIC
    TurboNet::Utils
    TurboNet::Parser
    uuid
)

cmake_config_target(
  ${TARGET_NAME}
  ALIAS TurboScript::DataBind
  FOLDER tbe/data_bind
  EXPORT_NAME DataBind)

install(FILES data_bind.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# =============================================================================
# Tests
# =============================================================================

add_executable(test_data_bind test_data_bind.c tbe_helpers.c)
target_include_directories(test_data_bind
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tbe/tbe/include
)
target_link_libraries(test_data_bind
  PRIVATE 
    TurboScript::DataBind
    TurboNet::TinyTest
)
set_target_properties(test_data_bind PROPERTIES FOLDER tbe/data_bind)
add_test(NAME test_data_bind COMMAND test_data_bind)

add_executable(test_data_bind_public_api test_data_bind_public_api.c)
target_link_libraries(test_data_bind_public_api
  PRIVATE
    TurboScript::DataBind
    TurboNet::TinyTest
)
set_target_properties(test_data_bind_public_api PROPERTIES FOLDER tbe/data_bind)
add_test(NAME test_data_bind_public_api COMMAND test_data_bind_public_api)
