/**
 * @file turbo_script_registry.c
 * @brief Registers TurboScript specific modules into the ExprTk global registry.
 */
#include "exprtk.h"
#include "exprtk_module.h"

#include "turbo_script_internal.h"

// Forward declarations for exprtk built-in modules
extern const exprtk_module_t *exprtk_module_math(void);
extern const exprtk_module_t *exprtk_module_string(void);
extern const exprtk_module_t *exprtk_module_stats(void);
extern const exprtk_module_t *exprtk_module_io(void);
extern const exprtk_module_t *exprtk_module_core(void);
extern const exprtk_module_t *exprtk_module_regex(void);

void turbo_script_register_modules(void) {
  exprtk_registry_add_module(exprtk_module_math());
  exprtk_registry_add_module(exprtk_module_string());
  exprtk_registry_add_module(exprtk_module_stats());
  exprtk_registry_add_module(exprtk_module_io());
  exprtk_registry_add_module(exprtk_module_core());
  exprtk_registry_add_module(exprtk_module_regex());

  exprtk_registry_init(); // Sort the registry
}
