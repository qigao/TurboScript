# DLL-based integration test — loads tbs_data_bind at runtime
cmake_add_test(
  TARGET test_data_bind_module
  SOURCES test_data_bind.c
  LIBS exprtk TurboUtils::TinyTest
  DEPENDS data_bind_tbs
  FOLDER modules/data_bind)

# Copy plugin DLL next to test executable so it can be found at runtime
add_custom_command(
  TARGET test_data_bind_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:data_bind_tbs>
          $<TARGET_FILE_DIR:test_data_bind_module>
)
