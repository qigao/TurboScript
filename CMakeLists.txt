cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# TurboNet Project - Unified High-Performance Networking Library
project(
  TurboScript
  VERSION 1.0.0
  LANGUAGES C CXX)

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

# Project description
set(PROJECT_DESCRIPTION "A simple DSL")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/turboscript")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17) # C++17 for compatibility
set(CMAKE_C_STANDARD 11) # C17 to ensure C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
include(CTest)
enable_testing()

include(CMakeOptions.cmake)
include(Sanitizers) 
include(GNUInstallDirs)
include(CmakeUtils)
 

find_package(Threads REQUIRED)
find_package(TurboNet CONFIG REQUIRED PATHS "${TURBONET_ROOT}")
find_package(TurboHttp CONFIG REQUIRED PATHS "${TURBOHTTP_ROOT}")
find_package(libfsm CONFIG REQUIRED PATHS "${FSM_ROOT}")
find_package(base64 CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(PkgConfig REQUIRED) 
pkg_check_modules(STC REQUIRED IMPORTED_TARGET stc)
find_path(SIMDE_INCLUDE_DIRS "simde/arm/neon.h")

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
#add_subdirectory(shared)
#add_subdirectory(parser)

add_subdirectory(exprtk) 
add_subdirectory(turbo_script) 
add_subdirectory(tbe)
add_subdirectory(modules) 
 

# Package Configuration
include(CMakePackageConfigHelpers)

# Install targets and export
install(EXPORT TurboScriptTargets
    FILE TurboScriptTargets.cmake
    NAMESPACE TurboScript::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboScript
)

# Generate project config
configure_package_config_file(
    cmake/TurboScriptConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboScript
    PATH_VARS
        CMAKE_INSTALL_BINDIR
        CMAKE_INSTALL_INCLUDEDIR
        CMAKE_INSTALL_LIBDIR
        TURBOSCRIPT_INSTALL_PLUGINDIR
)

write_basic_config_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboScript
)
 
