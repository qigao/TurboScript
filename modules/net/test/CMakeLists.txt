cmake_add_test(
  TARGET test_net_ctx
  SOURCES test_net_ctx.c
  LIBS TurboUtils::TinyTest exprtk ${TARGET_NAME_LIB} TurboUtils::Core
  FOLDER modules/net)

add_custom_command(
  TARGET test_net_ctx
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:TurboHttp::HttpClient> $<TARGET_FILE_DIR:test_net_ctx>)

cmake_add_test(
  TARGET test_net_module
  SOURCES test_net_plugin.c
  LIBS exprtk TurboUtils::TinyTest
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
