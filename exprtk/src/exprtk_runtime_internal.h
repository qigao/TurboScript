#ifndef EXPRTK_RUNTIME_INTERNAL_H
#define EXPRTK_RUNTIME_INTERNAL_H

#include "exprtk.h"

typedef enum exprtk_registration_status_e {
    EXPRTK_REGISTRATION_OK = 0,
    EXPRTK_REGISTRATION_INVALID_ARGUMENT = 1,
    EXPRTK_REGISTRATION_CONFLICT = 2,
    EXPRTK_REGISTRATION_OUT_OF_MEMORY = 3,
} exprtk_registration_status_t;

typedef struct exprtk_native_registration_s {
    const char *name;
    exprtk_native_fn fn;
    void *user_data;
} exprtk_native_registration_t;

typedef int (*exprtk_registration_fault_fn)(size_t allocation_index,
                                            void *user_data);

exprtk_registration_status_t exprtk_env_register_funcs_checked(
    exprtk_env_t *env, const exprtk_native_registration_t *registrations,
    size_t registration_count);

/* Internal deterministic fault-injection seam. It still uses the production
 * malloc/free owner and only suppresses the selected allocation attempt. */
exprtk_registration_status_t exprtk_env_register_funcs_checked_with_fault(
    exprtk_env_t *env, const exprtk_native_registration_t *registrations,
    size_t registration_count, exprtk_registration_fault_fn should_fail,
    void *fault_user_data);

#endif
