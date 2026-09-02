cmake_add_test(
  SOURCES test_timeseries.c test_ts_rolling.c
  LIBS Rocida::TinyTest ${TARGET_NAME} Rocida::Core
  FOLDER modules/ts)
