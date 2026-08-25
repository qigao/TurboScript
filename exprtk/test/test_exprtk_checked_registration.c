#include "tinytest.h"

#include "exprtk_runtime_internal.h"

typedef struct registration_fault_s {
    size_t fail_at;
    size_t calls;
} registration_fault_t;

static exprtk_value_t registration_noop(size_t arg_count, exprtk_value_t *args,
                                        exprtk_env_t *env, void *user_data) {
    (void)arg_count;
    (void)args;
    (void)env;
    (void)user_data;
    return exprtk_val_num(0.0);
}

static int fail_selected_allocation(size_t allocation_index, void *user_data) {
    registration_fault_t *fault = (registration_fault_t *)user_data;
    fault->calls++;
    return allocation_index == fault->fail_at;
}

static void check_transaction_failure_at(size_t fail_at) {
    exprtk_env_t env;
    exprtk_func_t *before;
    registration_fault_t fault = {fail_at, 0};
    const exprtk_native_registration_t batch[] = {
        {"batch.first", registration_noop, NULL},
        {"batch.middle", registration_noop, NULL},
        {"batch.last", registration_noop, NULL},
    };

    exprtk_env_init(&env);
    exprtk_env_register_func(&env, "sentinel", registration_noop, NULL);
    before = env.funcs;

    check_equal(exprtk_env_register_funcs_checked_with_fault(
                    &env, batch, sizeof(batch) / sizeof(batch[0]),
                    fail_selected_allocation, &fault),
                EXPRTK_REGISTRATION_OUT_OF_MEMORY);
    check_equal(fault.calls, fail_at + 1);
    check(env.funcs == before);
    check_equal(exprtk_env_has_func(&env, "sentinel"), 1);
    check_equal(exprtk_env_has_func(&env, "batch.first"), 0);
    check_equal(exprtk_env_has_func(&env, "batch.middle"), 0);
    check_equal(exprtk_env_has_func(&env, "batch.last"), 0);
    exprtk_env_free(&env);
}

spec("exprtk checked native registration") {
    it("rolls back when the first allocation fails") {
        check_transaction_failure_at(0);
    }

    it("rolls back when a middle allocation fails") {
        check_transaction_failure_at(3);
    }

    it("rolls back when the last allocation fails") {
        check_transaction_failure_at(5);
    }

    it("publishes the complete batch after every allocation succeeds") {
        exprtk_env_t env;
        const exprtk_native_registration_t batch[] = {
            {"batch.first", registration_noop, NULL},
            {"batch.middle", registration_noop, NULL},
            {"batch.last", registration_noop, NULL},
        };

        exprtk_env_init(&env);
        check_equal(exprtk_env_register_funcs_checked(
                        &env, batch, sizeof(batch) / sizeof(batch[0])),
                    EXPRTK_REGISTRATION_OK);
        check_equal(exprtk_env_has_func(&env, "batch.first"), 1);
        check_equal(exprtk_env_has_func(&env, "batch.middle"), 1);
        check_equal(exprtk_env_has_func(&env, "batch.last"), 1);
        exprtk_env_free(&env);
    }
}
