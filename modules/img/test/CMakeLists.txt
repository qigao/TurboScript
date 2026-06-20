set(TEST_NAME test_img)

add_executable(${TEST_NAME} test_img.c)

target_link_libraries(${TEST_NAME} PRIVATE img exprtk TurboNet::Utils TurboNet::TinyTest)

target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

if(UNIX)
  target_link_libraries(${TEST_NAME} PRIVATE m)
endif()

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

set_target_properties(${TEST_NAME} PROPERTIES FOLDER modules/img/test)
