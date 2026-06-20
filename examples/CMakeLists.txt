add_executable(example_wasm_module example_wasm_module.c)
target_include_directories(
  example_wasm_module
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../turbo_script/include
          ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/extra)
target_link_libraries(example_wasm_module PRIVATE turbo_script)
set_target_properties(example_wasm_module PROPERTIES FOLDER
                                                     turbo_script/examples)

# Copy wasm_plugin DLL/SO next to example so turbo_script_load_plugin("wasm")
# can find it.
add_custom_command(
  TARGET example_wasm_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:wasm_plugin>
          $<TARGET_FILE_DIR:example_wasm_module>)

add_test(NAME test_example_wasm_module_smoke COMMAND example_wasm_module)
set_tests_properties(
  test_example_wasm_module_smoke
  PROPERTIES WORKING_DIRECTORY $<TARGET_FILE_DIR:example_wasm_module>)
