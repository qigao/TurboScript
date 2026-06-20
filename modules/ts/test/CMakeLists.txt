add_executable(test_timeseries test_timeseries.c)
target_link_libraries(test_timeseries PRIVATE TurboNet::TinyTest ${TARGET_NAME}
                                              TurboNet::Utils)
add_test(NAME test_timeseries COMMAND test_timeseries)
set_target_properties(test_timeseries PROPERTIES FOLDER modules/ts)

add_executable(test_ts_rolling test_ts_rolling.c)
target_link_libraries(test_ts_rolling PRIVATE TurboNet::TinyTest ${TARGET_NAME}
                                              TurboNet::Utils)
add_test(NAME test_ts_rolling COMMAND test_ts_rolling)
set_target_properties(test_ts_rolling PROPERTIES FOLDER modules/ts)
