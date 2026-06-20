# DLL-based integration test — loads sqlite_plugin at runtime
add_executable(test_sqlite_module test_sqlite.c)
target_link_libraries(test_sqlite_module PRIVATE   exprtk
                                              TurboNet::TinyTest)
add_test(NAME test_sqlite_module COMMAND test_sqlite_module)
set_target_properties(test_sqlite_module PROPERTIES FOLDER
                                                 modules/sqlite)

# Copy plugin DLL next to test executable so it can be found at runtime
add_custom_command(
  TARGET test_sqlite_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sqlite_plugin>
          $<TARGET_FILE_DIR:test_sqlite_module>)
