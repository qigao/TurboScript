add_executable(turbo_script_repl main.c)
cmake_config_target(turbo_script_repl FOLDER turbo_script)

target_link_libraries(turbo_script_repl
  PRIVATE turbo_script TurboParser::Parser TurboNet::CoroNet)

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

  add_test(
    NAME example_polymarket_patterns_load
    COMMAND $<TARGET_FILE:turbo_script_repl>
            --file ${CMAKE_SOURCE_DIR}/examples/polymarket_patterns.tbs)
  set_tests_properties(example_polymarket_patterns_load
    PROPERTIES LABELS "examples;polymarket")

  add_test(
    NAME example_getting_started
    COMMAND $<TARGET_FILE:turbo_script_repl>
            --file ${CMAKE_SOURCE_DIR}/examples/getting_started.tbs)
  set_tests_properties(example_getting_started
    PROPERTIES LABELS "examples;getting-started")

  add_test(
    NAME turbo_script_cli_uses_actual_module_names
    COMMAND $<TARGET_FILE:turbo_script_repl>
            --eval "import(\"fin\"); import(\"ts\"); module_names_ok = 1;")
  set_tests_properties(turbo_script_cli_uses_actual_module_names
    PROPERTIES LABELS "turbo_script;cli;plugins")

  foreach(CORO_STACK_KIB IN ITEMS 0 512 1024)
    add_test(
      NAME turbo_script_cli_coro_stack_${CORO_STACK_KIB}
      COMMAND $<TARGET_FILE:turbo_script_repl>
              --coro-stack-kib ${CORO_STACK_KIB}
              --eval "stack_override = 1;")
    set_tests_properties(turbo_script_cli_coro_stack_${CORO_STACK_KIB}
      PROPERTIES LABELS "turbo_script;cli;coro")
  endforeach()

  add_test(
    NAME turbo_script_cli_coro_stack_rejects_small_value
    COMMAND $<TARGET_FILE:turbo_script_repl>
            --coro-stack-kib 1
            --eval "stack_override = 1;")
  set_tests_properties(turbo_script_cli_coro_stack_rejects_small_value
    PROPERTIES LABELS "turbo_script;cli;coro" WILL_FAIL TRUE)

  add_test(
    NAME turbo_script_cli_reports_batch_errors
    COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:turbo_script_repl>
            -DINVALID_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/test/invalid.tbs
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_cli_errors.cmake)
  set_tests_properties(turbo_script_cli_reports_batch_errors
    PROPERTIES LABELS "turbo_script;cli")
endif()
