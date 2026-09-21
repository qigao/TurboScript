cmake_add_test(
  TARGET test_net_ctx
  SOURCES test_net_ctx.c
  LIBS Salts::TinyTest exprtk ${TARGET_NAME_LIB} Salts::Core
  FOLDER modules/net)
