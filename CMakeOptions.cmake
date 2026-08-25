include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)

# if(MSVC)
#     add_compile_options(/bigobj)
# endif()

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(TURBO_SCRIPT_ENABLE_RULES_FORGE "Build the RulesForge TurboScript module" ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
