include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)

# if(MSVC)
#     add_compile_options(/bigobj)
# endif()

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(TURBOSCRIPT_BUILD_MODULES "Build the bundled native extension modules" ON)
option(TURBOSCRIPT_BUILD_CLI "Build the CLI and its bundled-module examples" ON)
if(TURBOSCRIPT_BUILD_CLI AND NOT TURBOSCRIPT_BUILD_MODULES)
  message(FATAL_ERROR
    "The CLI requires bundled modules. For embedding, explicitly set both TURBOSCRIPT_BUILD_MODULES=OFF and TURBOSCRIPT_BUILD_CLI=OFF.")
endif()
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
