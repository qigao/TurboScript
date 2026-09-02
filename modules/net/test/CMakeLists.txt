cmake_add_test(
  TARGET test_net_ctx
  SOURCES test_net_ctx.c
  LIBS Rocida::TinyTest exprtk ${TARGET_NAME_LIB} Rocida::Core
  FOLDER modules/net)

add_custom_command(
  TARGET test_net_ctx
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:TurboHttp::TurboHttp> $<TARGET_FILE_DIR:test_net_ctx>)

cmake_add_test(
  TARGET test_net_module
  SOURCES test_net_plugin.c
  LIBS exprtk Rocida::TinyTest
  DEPENDS net_tbs
  FOLDER modules/net)

add_custom_command(
  TARGET test_net_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:net_tbs>
          $<TARGET_FILE_DIR:test_net_module>)

if(TARGET TurboNet::CoroNet)
  add_custom_command(
    TARGET test_net_module
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TurboNet::CoroNet>
            $<TARGET_FILE_DIR:test_net_module>)
endif()

cmake_add_test(
  TARGET test_net_task_ws
  SOURCES test_net_task_ws.c
  LIBS turbo_script TurboNet::CoroNet Rocida::TinyTest
  DEPENDS net_tbs
  FOLDER modules/net)

add_custom_command(
  TARGET test_net_task_ws
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:net_tbs>
          $<TARGET_FILE_DIR:test_net_task_ws>)

cmake_add_test(
  TARGET test_net_task_http
  SOURCES test_net_task_http.c
  LIBS turbo_script TurboHttp::Iris TurboNet::CoroNet Rocida::TinyTest
  DEPENDS net_tbs
  FOLDER modules/net)

add_custom_command(
  TARGET test_net_task_http
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:net_tbs>
          $<TARGET_FILE_DIR:test_net_task_http>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TurboHttp::Iris>
          $<TARGET_FILE_DIR:test_net_task_http>)
