add_executable(test_turbo_script test_turbo_script.c)
target_link_libraries(test_turbo_script PRIVATE turbo_script TurboNet::TinyTest
                                                exprtk ta mir_static)

add_dependencies(test_turbo_script fin_plugin ta_plugin ts_plugin parser_plugin)

add_custom_command(
  TARGET test_turbo_script
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fin_plugin>
          $<TARGET_FILE_DIR:test_turbo_script>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ta_plugin>
          $<TARGET_FILE_DIR:test_turbo_script>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ts_plugin>
          $<TARGET_FILE_DIR:test_turbo_script>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:parser_plugin>
          $<TARGET_FILE_DIR:test_turbo_script>)
target_include_directories(test_turbo_script
                           PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
add_test(NAME test_turbo_script COMMAND test_turbo_script)
set_target_properties(test_turbo_script PROPERTIES FOLDER turbo_script)

add_executable(bench_turbo_script bench_turbo_script.c)
target_link_libraries(
  bench_turbo_script PRIVATE turbo_script_lib TurboNet::TinyTest exprtk
                             mir_static)
target_include_directories(bench_turbo_script
                           PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
add_test(NAME bench_turbo_script COMMAND bench_turbo_script)
set_target_properties(bench_turbo_script PROPERTIES FOLDER turbo_script)

add_executable(test_turbo_script_mir test_turbo_script_mir.c)
target_include_directories(test_turbo_script_mir
                           PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
target_link_libraries(test_turbo_script_mir
                      PRIVATE turbo_script TurboNet::TinyTest exprtk mir_static)
add_test(NAME test_turbo_script_mir COMMAND test_turbo_script_mir)
set_target_properties(test_turbo_script_mir PROPERTIES FOLDER turbo_script)

# JIT Performance Benchmarks
add_executable(bench_turbo_script_mir bench_turbo_script_mir.c)
target_include_directories(bench_turbo_script_mir
                           PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
target_link_libraries(bench_turbo_script_mir PRIVATE turbo_script_lib exprtk
                                                     mir_static)
set_target_properties(bench_turbo_script_mir PROPERTIES FOLDER turbo_script)
