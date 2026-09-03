cmake_add_test(
  SOURCES test_ta.c test_ta_advanced.c test_ta_edge.c
  LIBS Salts::TinyTest ${TARGET_NAME} Salts::Core
  FOLDER modules/ta)
