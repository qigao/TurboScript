cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# TurboScript Project - A Simple DSL with MIR JIT Backend
project(
  TurboScript
  VERSION 3.0.0
  LANGUAGES C CXX)

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

# Project description
set(PROJECT_DESCRIPTION "A simple DSL")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/TurboScript")

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
find_package(TurboUtils CONFIG REQUIRED PATHS "${TURBO_UTILS_ROOT}"
             NO_DEFAULT_PATH)
find_package(TurboNet CONFIG REQUIRED PATHS "${TURBONET_ROOT}" NO_DEFAULT_PATH)
find_package(TurboHttp CONFIG REQUIRED PATHS "${TURBOHTTP_ROOT}"
             NO_DEFAULT_PATH)

find_package(xxHash CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_path(SIMDE_INCLUDE_DIRS "simde/arm/neon.h")

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)

add_subdirectory(exprtk)
add_subdirectory(turbo_script)
add_subdirectory(modules)

# Package Configuration
include(CMakePackageConfigHelpers)

# Project-owned installation policy
install(
  TARGETS turbo_script
  EXPORT TurboScriptTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  TARGETS net_tbs
          ta_tbs
          fin_tbs
          ts_tbs
          sqlite_tbs
          mapper_tbs
          img_tbs
          coro_tbs
          parser_tbs
          crypto_tbs
          hash_tbs
          fuzzy_tbs
          rules_forge_tbs
          os_tbs
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  EXPORT TurboScriptTargets
  FILE TurboScriptTargets.cmake
  NAMESPACE TurboScript::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboScript)

# Generate project config
configure_package_config_file(
  cmake/TurboScriptConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboScript
  PATH_VARS CMAKE_INSTALL_BINDIR CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)

write_basic_config_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/TurboScriptConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboScript)
