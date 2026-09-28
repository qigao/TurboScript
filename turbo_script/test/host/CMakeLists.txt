# Host contracts that do not depend on bundled native modules.
# They exercise private seams in addition to the public Host ABI; the installed
# consumer under cmake/ci/host-abi separately verifies the package boundary.
foreach(suite IN ITEMS result registry module instance call state limits)
  cmake_add_test(
    TARGET test_turbo_script_host_${suite}
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../test_turbo_script_host_${suite}.c
    LIBS turbo_script_lib Salts::TinyTest Salts::CSTL exprtk MIR::MIR
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../src/host
             ${CMAKE_CURRENT_SOURCE_DIR}/../../src/mir
    FOLDER turbo_script)
endforeach()
