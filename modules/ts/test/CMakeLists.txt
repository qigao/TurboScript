cmake_add_test(
  SOURCES test_timeseries.c test_ts_rolling.c
  LIBS TurboUtils::TinyTest ${TARGET_NAME} TurboUtils::Core
  FOLDER modules/ts)
