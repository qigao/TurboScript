cmake_add_test(
  TARGET test_net_ctx
  SOURCES test_net_ctx.c
  LIBS Salts::TinyTest exprtk ${TARGET_NAME_LIB} Salts::Core
  FOLDER modules/net)

add_custom_command(
  TARGET test_net_ctx
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:Salts::CHTTP> $<TARGET_FILE_DIR:test_net_ctx>)
