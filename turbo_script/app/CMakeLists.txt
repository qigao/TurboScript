add_executable(turbo_script_repl main.c)

target_link_libraries(turbo_script_repl
  PRIVATE turbo_script TurboNet::Parser exprtk)

set_target_properties(turbo_script_repl PROPERTIES FOLDER turbo_script)

add_dependencies(turbo_script_repl fin_plugin ta_plugin ts_plugin parser_plugin coro_plugin)

add_custom_command(
  TARGET turbo_script_repl
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fin_plugin> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ta_plugin> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ts_plugin> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:parser_plugin> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:coro_plugin> $<TARGET_FILE_DIR:turbo_script_repl>
)
