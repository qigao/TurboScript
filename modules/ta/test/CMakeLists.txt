cmake_add_test(
  SOURCES test_ta.c test_ta_advanced.c test_ta_edge.c
  LIBS Rocida::TinyTest ${TARGET_NAME} Rocida::Core
  FOLDER modules/ta)
