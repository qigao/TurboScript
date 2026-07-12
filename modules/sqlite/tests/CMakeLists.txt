# DLL-based integration test — loads tbs_sqlite at runtime
cmake_add_test(
  TARGET test_sqlite_module
  SOURCES test_sqlite.c
  LIBS exprtk TurboUtils::TinyTest
  DEPENDS sqlite_tbs
  FOLDER modules/sqlite)

# Copy plugin DLL next to test executable so it can be found at runtime
add_custom_command(
  TARGET test_sqlite_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sqlite_tbs>
          $<TARGET_FILE_DIR:test_sqlite_module>)
