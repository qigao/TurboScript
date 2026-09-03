cmake_add_test(
  SOURCES test_timeseries.c test_ts_rolling.c
  LIBS Salts::TinyTest ${TARGET_NAME} Salts::Core
  FOLDER modules/ts)
