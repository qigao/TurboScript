add_executable(test_ta test_ta.c)
target_link_libraries(test_ta PRIVATE TurboUtils::TinyTest ${TARGET_NAME}
                                      TurboUtils::Core)
add_test(NAME test_ta COMMAND test_ta)
set_target_properties(test_ta PROPERTIES FOLDER modules/ta)

add_executable(test_ta_advanced test_ta_advanced.c)
target_link_libraries(test_ta_advanced PRIVATE TurboUtils::TinyTest ${TARGET_NAME}
                                               TurboUtils::Core)
add_test(NAME test_ta_advanced COMMAND test_ta_advanced)
set_target_properties(test_ta_advanced PROPERTIES FOLDER modules/ta)

add_executable(test_ta_edge test_ta_edge.c)
target_link_libraries(test_ta_edge PRIVATE TurboUtils::TinyTest ${TARGET_NAME}
                                           TurboUtils::Core)
add_test(NAME test_ta_edge COMMAND test_ta_edge)
set_target_properties(test_ta_edge PROPERTIES FOLDER modules/ta)
