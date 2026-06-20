add_library(miniblas_linalg STATIC miniblas.c linalg.c miniblas.h linalg.h)

target_include_directories(
  miniblas_linalg PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

find_path(SIMDE_INCLUDE_DIRS "simde/simde-common.h")
if(SIMDE_INCLUDE_DIRS)
  target_include_directories(miniblas_linalg PRIVATE ${SIMDE_INCLUDE_DIRS})
  target_compile_definitions(miniblas_linalg PRIVATE MINIBLAS_USE_SIMDE)
endif()

set_target_properties(
  miniblas_linalg
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             FOLDER vendor)
