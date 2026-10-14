cmake_add_test(
  TARGET test_mapper_module
  TEST_NAME mapper_module_tests
  SOURCES test_mapper.c
  LIBS mapper exprtk Salts::TinyTest Salts::Core Salts::DataBind
  FOLDER modules/mapper
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The shared Mapper plugin must resolve its own static CSTL code. The module
# API cannot borrow a hidden implementation from libsalts or its loading host.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_mapper_plugin_dlopen test_mapper_plugin_dlopen.c)
  target_link_libraries(test_mapper_plugin_dlopen PRIVATE ${CMAKE_DL_LIBS})
  add_test(
    NAME mapper_plugin_real_dlopen
    COMMAND test_mapper_plugin_dlopen "$<TARGET_FILE:mapper_tbs>")
  add_test(
    NAME mapper_plugin_private_cstl_symbols
    COMMAND "${CMAKE_COMMAND}"
      "-DMAPPER_PLUGIN_IMAGE=$<TARGET_FILE:mapper_tbs>"
      "-DCMAKE_NM_TOOL=${CMAKE_NM}"
      -P "${CMAKE_SOURCE_DIR}/cmake/ci/verify_mapper_private_cstl.cmake")
endif()
