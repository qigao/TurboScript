# These existing host contracts exercise private seams as well as the public ABI.
# They do not use the optional TA/native plugins. Installed consumers below test
# the shared-library/public-header boundary independently.
foreach(suite IN ITEMS result registry module instance call state limits)
  cmake_add_test(
    TARGET test_turbo_script_host_${suite}
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../test_turbo_script_host_${suite}.c
    LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk mir_static
    INCLUDES ${CMAKE_SOURCE_DIR}/vendor/mir
             ${CMAKE_CURRENT_SOURCE_DIR}/../../src/host
             ${CMAKE_CURRENT_SOURCE_DIR}/../../src/mir
    FOLDER turbo_script)
endforeach()
