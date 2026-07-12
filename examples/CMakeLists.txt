cmake_add_test(
  TARGET example_wasm_module
  TEST_NAME test_example_wasm_module_smoke
  SOURCES example_wasm_module.c
  FOLDER turbo_script/examples)
target_include_directories(
  example_wasm_module
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../turbo_script/include
          ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/extra)
target_link_libraries(example_wasm_module PRIVATE turbo_script)
# Copy wasm_tbs DLL/SO next to example so turbo_script_load_plugin("wasm")
# can find it.
add_custom_command(
  TARGET example_wasm_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:wasm_tbs>
          $<TARGET_FILE_DIR:example_wasm_module>)

set_tests_properties(
  test_example_wasm_module_smoke
  PROPERTIES WORKING_DIRECTORY $<TARGET_FILE_DIR:example_wasm_module>)
