cmake_add_test(
  TARGET test_rules_forge_plugin
  SOURCES test_rules_forge_plugin.c
  LIBS exprtk TurboUtils::TinyTest
  DEPENDS rules_forge_tbs
  FOLDER modules/rules_forge)
if(WIN32)
  get_filename_component(RULES_FORGE_DEPS_ROOT "${RulesForge_ROOT}/../turboutils/bin" ABSOLUTE)
  if(NOT IS_DIRECTORY "${RULES_FORGE_DEPS_ROOT}")
    message(FATAL_ERROR "RulesForge runtime dependency directory not found: ${RULES_FORGE_DEPS_ROOT}")
  endif()
  set_tests_properties(test_rules_forge_plugin PROPERTIES
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${RulesForge_ROOT}/bin;PATH=path_list_prepend:${RULES_FORGE_DEPS_ROOT}")
endif()
