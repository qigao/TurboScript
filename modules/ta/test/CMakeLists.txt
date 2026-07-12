cmake_add_test(
  SOURCES test_ta.c test_ta_advanced.c test_ta_edge.c
  LIBS TurboUtils::TinyTest ${TARGET_NAME} TurboUtils::Core
  FOLDER modules/ta)
