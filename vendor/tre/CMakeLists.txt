set(TRE_VERSION "0.9.0")
set(TRE_VERSION_1 0)
set(TRE_VERSION_2 9)
set(TRE_VERSION_3 0)

configure_file(cmake/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)
configure_file(cmake/tre-config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/tre/tre-config.h @ONLY)

set(TRE_SOURCES
    lib/tre-ast.c
    lib/tre-compile.c
    lib/tre-match-backtrack.c
    lib/tre-match-parallel.c
    lib/tre-match-approx.c
    lib/tre-mem.c
    lib/tre-parse.c
    lib/tre-stack.c
    lib/regcomp.c
    lib/regexec.c
    lib/regerror.c)

set(TRE_HEADERS
    include/tre/tre.h
    include/tre/regex.h
    lib/tre-ast.h
    lib/tre-compile.h
    lib/tre-internal.h
    lib/tre-match-utils.h
    lib/tre-mem.h
    lib/tre-parse.h
    lib/tre-stack.h
    lib/xmalloc.h)

add_library(tre STATIC ${TRE_SOURCES} ${TRE_HEADERS})

cmake_config_target(
  tre
  ALIAS TurboScript::Tre
  FOLDER vendor
  EXPORT_NAME Tre
  NO_INSTALL)

target_compile_definitions(tre PRIVATE HAVE_CONFIG_H)

if(MSVC)
  target_compile_definitions(tre PRIVATE _CRT_SECURE_NO_WARNINGS
                                         _CRT_SECURE_NO_DEPRECATE)
endif()

target_include_directories(
  tre
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include/tre>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/lib)

set_target_properties(tre PROPERTIES POSITION_INDEPENDENT_CODE ON)
