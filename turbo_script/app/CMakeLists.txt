add_executable(turbo_script_repl main.c)
cmake_config_target(turbo_script_repl FOLDER turbo_script)

target_link_libraries(turbo_script_repl
  PRIVATE turbo_script TurboUtils::Parser TurboNet::CoroNet)

add_dependencies(turbo_script_repl fin_tbs ta_tbs ts_tbs parser_tbs coro_tbs)

add_custom_command(
  TARGET turbo_script_repl
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fin_tbs> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ta_tbs> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ts_tbs> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:parser_tbs> $<TARGET_FILE_DIR:turbo_script_repl>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:coro_tbs> $<TARGET_FILE_DIR:turbo_script_repl>
)

if(BUILD_TESTING)
  set(TURBO_SCRIPT_GOF_EXAMPLES
    factory_builder.tbs
    adapter_decorator.tbs
    composite_visitor.tbs
    command_memento.tbs
    state_chain.tbs)

  foreach(EXAMPLE_FILE IN LISTS TURBO_SCRIPT_GOF_EXAMPLES)
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_FILE} NAME_WE)
    add_test(
      NAME example_gof_${EXAMPLE_NAME}
      COMMAND $<TARGET_FILE:turbo_script_repl>
              --file ${CMAKE_SOURCE_DIR}/examples/gof/${EXAMPLE_FILE})
    set_tests_properties(example_gof_${EXAMPLE_NAME}
      PROPERTIES LABELS "examples;gof")
  endforeach()
endif()
