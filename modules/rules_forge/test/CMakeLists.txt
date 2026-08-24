cmake_add_test(
  TARGET test_rules_forge_plugin
  SOURCES test_rules_forge_plugin.c
  LIBS exprtk TurboUtils::TinyTest
  DEPENDS rules_forge_tbs
  FOLDER modules/rules_forge)
if(WIN32)
  set_tests_properties(test_rules_forge_plugin PROPERTIES
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$ENV{RULES_FORGE_ROOT}/bin;PATH=path_list_prepend:$ENV{TURBOUTILS_ROOT}/bin")
endif()
