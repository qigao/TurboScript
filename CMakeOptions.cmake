include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)

# if(MSVC)
#     add_compile_options(/bigobj)
# endif()

option(TURBOSCRIPT_BUILD_HOST_TOOLS_ONLY "Build only native parser generators for cross compilation" OFF)

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(TURBOSCRIPT_BUILD_HOST_TESTS "Build Host ABI contract suites without bundled modules" OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(TURBOSCRIPT_BUILD_MODULES "Build the bundled native extension modules" ON)
option(TURBOSCRIPT_BUILD_CLI "Build the CLI and its bundled-module examples" ON)
if(TURBOSCRIPT_BUILD_CLI AND NOT TURBOSCRIPT_BUILD_MODULES)
  message(FATAL_ERROR "The CLI requires bundled modules; disable both options for an embedded SDK")
endif()
if(BUILD_TESTS AND ENABLE_TESTS AND NOT TURBOSCRIPT_BUILD_MODULES)
  message(FATAL_ERROR "The full test suite requires bundled modules; embedded SDK builds must explicitly disable BUILD_TESTS")
endif()
