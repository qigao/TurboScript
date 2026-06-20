add_executable(test_net_ctx test_net_ctx.c)
target_link_libraries(test_net_ctx PRIVATE TurboNet::TinyTest exprtk
                                           ${TARGET_NAME_LIB}
                                           PUBLIC TurboNet::Utils)
add_test(NAME test_net_ctx COMMAND test_net_ctx)
set_target_properties(test_net_ctx PROPERTIES FOLDER modules/net)

add_executable(test_net_module test_net_plugin.c)
target_link_libraries(test_net_module PRIVATE exprtk TurboNet::TinyTest)
add_test(NAME test_net_module COMMAND test_net_module)
set_target_properties(test_net_module PROPERTIES FOLDER modules/net)

add_custom_command(
  TARGET test_net_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:net_plugin>
          $<TARGET_FILE_DIR:test_net_module>)
