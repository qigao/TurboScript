cmake_add_test(
  TARGET test_img
  SOURCES test_img.c
  LIBS img exprtk Rocida::Core Rocida::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER modules/img/test)

if(UNIX)
  target_link_libraries(test_img PRIVATE m)
endif()
