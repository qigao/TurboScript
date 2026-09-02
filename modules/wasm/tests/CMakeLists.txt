# DLL-based integration test — loads wasm at runtime
cmake_add_test(
  TARGET test_wasm_module
  SOURCES test_wasm.c
  LIBS exprtk Rocida::TinyTest
  INCLUDES ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/extra
  DEPENDS wasm_tbs
  FOLDER modules/wasm)

# Copy plugin DLL next to test executable so it can be found at runtime
add_custom_command(
  TARGET test_wasm_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:wasm_tbs>
          $<TARGET_FILE_DIR:test_wasm_module>)
