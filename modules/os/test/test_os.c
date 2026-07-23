#include "os.h"
#include "exprtk.h"
#include "tinytest.h"

#include <string.h>

void *os_module_create(void);
void os_module_load(void *module, void *env, void *scratch);
void os_module_destroy(void *module);

static exprtk_builtin_fn find_function(const exprtk_module_t *module, const char *name) {
    size_t i;

    for (i = 0; i < module->count; ++i) {
        if (strcmp(module->entries[i].name, name) == 0) return module->entries[i].fn;
    }
    return NULL;
}

spec("os_module") {
    describe("platform") {
        it("returns the host platform name") {
            mem_pool_t arena;
            exprtk_builtin_fn fn = find_function(exprtk_module_os(), "platform_name");
            exprtk_value_t result;

            check_not_null(fn);
            check_int_eq(mem_init(&arena, 4096), 0);
            result = fn(0, NULL, NULL, &arena);
            check_int_eq(result.type, EXPRTK_VAL_STRING);
            check(result.data.string.len > 0);
            mem_destroy(&arena);
        }

        it("rejects unexpected arguments") {
            mem_pool_t arena;
            exprtk_builtin_fn fn = find_function(exprtk_module_os(), "arch");
            exprtk_value_t arg = exprtk_val_num(1);
            exprtk_value_t result;

            check_not_null(fn);
            check_int_eq(mem_init(&arena, 4096), 0);
            result = fn(1, &arg, NULL, &arena);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            mem_destroy(&arena);
        }
    }

    describe("process lifecycle") {
        it("starts, waits for, reads, and closes a child process") {
            exprtk_env_t env;
            exprtk_value_t start_args[3];
            exprtk_value_t wait_args[1];
            exprtk_value_t result;
            exprtk_value_t field;
            void *module;
            int64_t process_id;

            exprtk_env_init(&env);
            module = os_module_create();
            check_not_null(module);
            os_module_load(module, &env, NULL);

#ifdef _WIN32
            start_args[0] = exprtk_val_str(tstr_v_from_cstr("cmd.exe"));
            start_args[1] = exprtk_val_str(tstr_v_from_cstr("/c"));
            start_args[2] = exprtk_val_str(tstr_v_from_cstr("echo os-module"));
            result = exprtk_call_internal("os.process_start", 3, start_args, &env);
#else
            start_args[0] = exprtk_val_str(tstr_v_from_cstr("/bin/echo"));
            start_args[1] = exprtk_val_str(tstr_v_from_cstr("os-module"));
            result = exprtk_call_internal("os.process_start", 2, start_args, &env);
#endif
            check_int_eq(result.type, EXPRTK_VAL_INTEGER);
            check(result.data.integer > 0);
            process_id = result.data.integer;
            exprtk_value_destroy(&result);

            wait_args[0] = exprtk_val_int(process_id);
            result = exprtk_call_internal("os.process_wait", 1, wait_args, &env);
            check_int_eq(result.type, EXPRTK_VAL_MAP);
            field = exprtk_map_get(&result, "running");
            check_int_eq(field.type, EXPRTK_VAL_NUMBER);
            check_float_eq(field.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);

            result = exprtk_call_internal("os.process_read_stdout", 1, wait_args, &env);
            check_int_eq(result.type, EXPRTK_VAL_STRING);
            check_str_contains(result.data.string.data, "os-module");
            exprtk_value_destroy(&result);

            result = exprtk_call_internal("os.process_close", 1, wait_args, &env);
            check_float_eq(result.data.number, 1.0, 0.001);
            exprtk_value_destroy(&result);
            os_module_destroy(module);
            exprtk_env_free(&env);
        }
    }

    describe("service controls") {
        it("reject empty service names before invoking the service manager") {
            exprtk_env_t env;
            exprtk_value_t empty_name = exprtk_val_str(tstr_v_from_cstr(""));
            exprtk_value_t result;
            void *module;

            exprtk_env_init(&env);
            module = os_module_create();
            check_not_null(module);
            os_module_load(module, &env, NULL);

            result = exprtk_call_internal("os.service_start", 1, &empty_name, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);

            result = exprtk_call_internal("os.service_stop", 1, &empty_name, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);

            result = exprtk_call_internal("os.reboot", 1, &empty_name, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);

            result = exprtk_call_internal("os.shutdown", 1, &empty_name, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);

            os_module_destroy(module);
            exprtk_env_free(&env);
        }
    }

    describe("power schedules") {
        it("creates, reports, and cancels an in-process cron schedule") {
            exprtk_env_t env;
            exprtk_value_t args[2];
            exprtk_value_t cancel_arg;
            exprtk_value_t result;
            exprtk_value_t field;
            void *module;
            int64_t schedule_id;

            exprtk_env_init(&env);
            module = os_module_create();
            check_not_null(module);
            os_module_load(module, &env, NULL);

            args[0] = exprtk_val_str(tstr_v_from_cstr("reboot"));
            args[1] = exprtk_val_str(tstr_v_from_cstr("0 0 1 1 *"));
            result = exprtk_call_internal("os.power_schedule", 2, args, &env);
            check_int_eq(result.type, EXPRTK_VAL_INTEGER);
            check(result.data.integer > 0);
            schedule_id = result.data.integer;
            exprtk_value_destroy(&result);

            cancel_arg = exprtk_val_int(schedule_id);
            result = exprtk_call_internal("os.power_schedule_status", 1, &cancel_arg, &env);
            check_int_eq(result.type, EXPRTK_VAL_MAP);
            field = exprtk_map_get(&result, "active");
            check_int_eq(field.type, EXPRTK_VAL_NUMBER);
            check_float_eq(field.data.number, 1.0, 0.001);
            field = exprtk_map_get(&result, "fire_count");
            check_int_eq(field.type, EXPRTK_VAL_INTEGER);
            check_int_eq(field.data.integer, 0);
            exprtk_value_destroy(&result);

            result = exprtk_call_internal("os.power_schedule_cancel", 1, &cancel_arg, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 1.0, 0.001);
            exprtk_value_destroy(&result);

            result = exprtk_call_internal("os.power_schedule_status", 1, &cancel_arg, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);
            os_module_destroy(module);
            exprtk_env_free(&env);
        }

        it("rejects invalid actions and cron expressions") {
            exprtk_env_t env;
            exprtk_value_t args[2];
            exprtk_value_t result;
            void *module;

            exprtk_env_init(&env);
            module = os_module_create();
            check_not_null(module);
            os_module_load(module, &env, NULL);

            args[0] = exprtk_val_str(tstr_v_from_cstr("hibernate"));
            args[1] = exprtk_val_str(tstr_v_from_cstr("0 0 1 1 *"));
            result = exprtk_call_internal("os.power_schedule", 2, args, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);

            args[0] = exprtk_val_str(tstr_v_from_cstr("shutdown"));
            args[1] = exprtk_val_str(tstr_v_from_cstr("invalid cron"));
            result = exprtk_call_internal("os.power_schedule", 2, args, &env);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_float_eq(result.data.number, 0.0, 0.001);
            exprtk_value_destroy(&result);
            os_module_destroy(module);
            exprtk_env_free(&env);
        }
    }
}
