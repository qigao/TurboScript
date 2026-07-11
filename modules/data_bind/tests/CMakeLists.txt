# DLL-based integration test — loads data_bind_tbs at runtime
add_executable(test_data_bind_module test_data_bind.c)
target_link_libraries(test_data_bind_module PRIVATE
  exprtk
  TurboUtils::TinyTest
)
add_test(NAME test_data_bind_module COMMAND test_data_bind_module)
set_target_properties(test_data_bind_module PROPERTIES FOLDER modules/data_bind)

# Copy plugin DLL next to test executable so it can be found at runtime
add_custom_command(
  TARGET test_data_bind_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:data_bind_tbs>
          $<TARGET_FILE_DIR:test_data_bind_module>
)
