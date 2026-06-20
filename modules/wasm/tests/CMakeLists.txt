# DLL-based integration test — loads wasm_plugin at runtime
add_executable(test_wasm_module test_wasm.c)
target_link_libraries(test_wasm_module PRIVATE   exprtk TurboNet::TinyTest)
target_include_directories(test_wasm_module PRIVATE ${CMAKE_SOURCE_DIR}/vendor/wasm3/source/extra)
add_test(NAME test_wasm_module COMMAND test_wasm_module)
set_target_properties(test_wasm_module PROPERTIES FOLDER modules/wasm)

# Copy plugin DLL next to test executable so it can be found at runtime
add_custom_command(
  TARGET test_wasm_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:wasm_plugin>
          $<TARGET_FILE_DIR:test_wasm_module>)

