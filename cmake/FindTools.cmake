# Lexer generation must use the explicitly selected host SDK.
if(NOT DEFINED ENV{RE2C_ROOT} OR NOT IS_DIRECTORY "$ENV{RE2C_ROOT}")
    message(FATAL_ERROR "RE2C_ROOT must name the restored host re2c package")
endif()
unset(RE2C_EXECUTABLE CACHE)
unset(RE2C_EXECUTABLE)
file(TO_CMAKE_PATH "$ENV{RE2C_ROOT}" _re2c_root)
find_program(RE2C_EXECUTABLE re2c PATHS "${_re2c_root}/bin"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
file(REAL_PATH "$ENV{RE2C_ROOT}" _re2c_root)
file(REAL_PATH "${RE2C_EXECUTABLE}" _re2c_executable)
cmake_path(IS_PREFIX _re2c_root "${_re2c_executable}" NORMALIZE _re2c_in_root)
if(NOT _re2c_in_root)
    message(FATAL_ERROR "re2c executable is outside RE2C_ROOT: ${RE2C_EXECUTABLE}")
endif()

# The bundled parser generator and template must stay in sync.
if(NOT TARGET lemon)
    message(FATAL_ERROR "The required in-tree lemon target is missing")
endif()
set(LEMON_EXECUTABLE $<TARGET_FILE:lemon>)
set(LEMON_DEPENDS lemon)

# Set path to lemon parser template
set(LEMPAR "${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c" CACHE PATH "Path to lemon parser template")

# Note: These variables are set in the root scope and will be inherited 
# by all subdirectories added via add_subdirectory().

message(STATUS "Tools detection:")
message(STATUS "  re2c: ${RE2C_EXECUTABLE}")
message(STATUS "  lemon: ${LEMON_EXECUTABLE}")
message(STATUS "  lempar: ${LEMPAR}")
