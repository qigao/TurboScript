add_executable(test_rules_forge_plugin test_rules_forge_plugin.c)

target_link_libraries(test_rules_forge_plugin PRIVATE exprtk TurboNet::TinyTest)

add_test(NAME test_rules_forge_plugin COMMAND test_rules_forge_plugin)
set_target_properties(test_rules_forge_plugin PROPERTIES FOLDER modules/rules_forge)

add_custom_command(
  TARGET test_rules_forge_plugin
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rules_forge_plugin>
          $<TARGET_FILE_DIR:test_rules_forge_plugin>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:RulesForge::rule_forge>
          $<TARGET_FILE_DIR:test_rules_forge_plugin>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:data_bind>
          $<TARGET_FILE_DIR:test_rules_forge_plugin>)
