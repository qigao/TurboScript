cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# TurboScript Project - A Simple DSL with MIR JIT Backend
project(
  TurboScript
  VERSION 3.0.10
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

if(TURBOSCRIPT_BUILD_HOST_TOOLS_ONLY)
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Parser generators must be built for the host")
  endif()
  add_subdirectory(tools)
  return()
endif()

find_package(Threads REQUIRED)
set(_turboscript_sdk_roots SALTS_ROOT SALTS_UTILS_ROOT)
if(TURBOSCRIPT_BUILD_MODULES)
  list(APPEND _turboscript_sdk_roots CHTTP_ROOT)
endif()
foreach(_root IN LISTS _turboscript_sdk_roots)
  if(NOT DEFINED ENV{${_root}} OR NOT IS_DIRECTORY "$ENV{${_root}}")
    message(FATAL_ERROR "${_root} must name the matching installed SDK; run cmake/ci/restore-native-sdks.ps1 first")
  endif()
  file(TO_CMAKE_PATH "$ENV{${_root}}" _turboscript_${_root})
endforeach()
# A previous configure must not select a different SDK than the active roots.
foreach(_package IN ITEMS Salts SaltsUtils Chttp TurboDB)
  unset(${_package}_DIR CACHE)
  unset(${_package}_DIR)
endforeach()
find_package(Salts CONFIG REQUIRED PATHS "${_turboscript_SALTS_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED PATHS "${_turboscript_SALTS_UTILS_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
set(TURBOSCRIPT_HAS_TURBODB OFF)
if(TURBOSCRIPT_BUILD_MODULES)
  find_package(Chttp CONFIG REQUIRED PATHS "${_turboscript_CHTTP_ROOT}"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(EXISTS "$ENV{TURBODB_ROOT}/lib/cmake/TurboDB/TurboDBConfig.cmake")
    file(TO_CMAKE_PATH "$ENV{TURBODB_ROOT}" _turboscript_TURBODB_ROOT)
    find_package(TurboDB CONFIG REQUIRED PATHS "${_turboscript_TURBODB_ROOT}"
                 NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    set(TURBOSCRIPT_HAS_TURBODB ON)
  endif()
endif()
find_package(MIR CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_path(SIMDE_INCLUDE_DIRS "simde/arm/neon.h")

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
add_subdirectory(miniblas)

add_subdirectory(exprtk)
add_subdirectory(turbo_script)
if(TURBOSCRIPT_BUILD_MODULES)
  add_subdirectory(modules)
endif()

# Package Configuration
include(CMakePackageConfigHelpers)

# Project-owned installation policy
install(
  TARGETS turbo_script exprtk exprtk_syntax miniblas_linalg
  EXPORT TurboScriptTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TURBOSCRIPT_BUILD_MODULES)
set(_turboscript_module_targets
    net_tbs
    ta_tbs
    fin_tbs
    ts_tbs
    mapper_tbs
    img_tbs
    crypto_tbs
    hash_tbs
    fuzzy_tbs
    os_tbs)
if(TARGET db_tbs)
  list(APPEND _turboscript_module_targets db_tbs)
endif()
install(
  TARGETS ${_turboscript_module_targets}
  LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins)
unset(_turboscript_module_targets)
endif()

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

install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/TurboScript)
