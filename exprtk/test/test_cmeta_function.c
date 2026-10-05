#include "exprtk.h"
#include "exprtk_class.h"
#include "exprtk_module.h"
#include "tinytest.h"

#include <string.h>

extern const exprtk_module_t *exprtk_module_math(void);

static exprtk_value_t scratch_string_builtin(
    size_t argc, exprtk_value_t *args,
    exprtk_env_t *env, mem_pool_t *scratch) {
    static const char text[] = "scratch-owned";
    char *copy;
    (void)argc;
    (void)args;
    (void)env;

    copy = (char *)mem_alloc(scratch, sizeof(text));
    if (!copy) return exprtk_val_num(0.0);
    memcpy(copy, text, sizeof(text));
    return exprtk_val_str(vstr_from_buf(copy, sizeof(text) - 1u));
}

spec("CMeta builtin function reflection") {
    it("uses one canonical exprtk value descriptor across reflection") {
        const cmeta_type_desc *value_type = exprtk_value_cmeta_type();
        const cmeta_type_desc *value_ptr_type = exprtk_value_ptr_cmeta_type();
        const cmeta_data_desc *value_data = exprtk_value_cmeta_data();
        const exprtk_module_t *module = exprtk_module_math();
        exprtk_function_reflection_t reflected = {0};

        check_not_null(value_type);
        check_not_null(value_ptr_type);
        check_not_null(value_data);
        check(cmeta_type_desc_valid(value_type));
        check(cmeta_type_desc_valid(value_ptr_type));
        check(cmeta_data_desc_valid(value_data));
        check_equal(value_type->name, "exprtk_value_t");
        check_equal(
            value_type->identity->stable_atom_id,
            "turboscript.exprtk.value");
        check(value_ptr_type->pointee == value_type);
        check(value_data->storage_type == value_type);
        check_equal(value_data->stable_id, "turboscript.exprtk.value.data");
        check_not_null(cmeta_data_construct_ops_of(value_data));
        check_true(cmeta_data_value_move_supported(value_data));
        check_false(cmeta_data_value_copy_supported(value_data));

        check_not_null(module);
        check(module->count > 0u);
        check(exprtk_module_function_reflect(module, 0u, &reflected));
        check(reflected.function.return_type == value_type);
        check(reflected.function.params[1].type == value_ptr_type);
    }

    it("projects the exact native VM adapter as a canonical FunctionDesc") {
        const exprtk_module_t *module = exprtk_module_math();
        exprtk_function_reflection_t reflected = {0};
        const cmeta_function_desc *fn;

        check_not_null(module);
        check(module->count > 0u);
        check(exprtk_module_function_reflect(module, 0u, &reflected));

        fn = &reflected.function;
        check(cmeta_function_desc_valid(fn));
        check_equal(fn->name, module->entries[0].name);
        check((fn->param_count) == (4u));
        check_equal(fn->return_type->name, "exprtk_value_t");
        check((fn->effects) == (CMETA_EFFECT_UNKNOWN));
        check((fn->properties) == (CMETA_PROP_NONE));
        check((fn->result_flags) == (CMETA_RESULT_UNKNOWN));
        check((reflected.invoke) == (module->entries[0].fn));

        check_equal(fn->params[0].name, "argc");
        check_equal(fn->params[0].type->name, "size_t");
        check_equal(fn->params[1].name, "args");
        check_equal(fn->params[1].type->name, "exprtk_value_t *");
        check((fn->params[1].flags & CMETA_PARAM_NULLABLE) != 0u);
        check_equal(fn->params[2].name, "env");
        check_equal(fn->params[2].type->name, "exprtk_env_t *");
        check_equal(fn->params[3].name, "scratch");
        check_equal(fn->params[3].type->name, "mem_pool_t *");
    }

    it("produces semantically equal descriptors on repeated admission") {
        const exprtk_module_t *module = exprtk_module_math();
        exprtk_function_reflection_t left = {0};
        exprtk_function_reflection_t right = {0};

        check(exprtk_module_function_reflect(module, 0u, &left));
        check(exprtk_module_function_reflect(module, 0u, &right));
        check(cmeta_function_desc_equal(&left.function, &right.function));
        check((left.invoke) == (right.invoke));
    }

    it("resolves registry builtins through the same admitted invoke authority") {
        exprtk_env_t env;
        const exprtk_module_t *module = exprtk_module_math();
        exprtk_function_reflection_t left = {0};
        exprtk_function_reflection_t right = {0};
        const char *name;

        check_not_null(module);
        check(module->count > 0u);
        name = module->entries[0].name;
        check_not_null(name);

        exprtk_env_init(&env);
        exprtk_env_add_module(&env, module);

        check(exprtk_find_builtin_reflection(name, &env, &left));
        check(exprtk_find_builtin_reflection(name, &env, &right));
        check(cmeta_function_desc_valid(&left.function));
        check(cmeta_function_desc_equal(&left.function, &right.function));
        check((left.invoke) == (module->entries[0].fn));
        check((left.invoke) == (exprtk_find_builtin(name, &env)));
        check((right.invoke) == (left.invoke));

        exprtk_env_free(&env);
    }

    it("promotes scratch-backed raw builtin results before scratch teardown") {
        exprtk_env_t env;
        exprtk_value_t result;

        exprtk_env_init(&env);
        result = exprtk_call_builtin(scratch_string_builtin, 0u, NULL, &env);

        check((result.type) == (EXPRTK_VAL_STRING));
        check((result.ownership) == (EXPRTK_VALUE_OWNED));
        check_not_null(result.storage);
        check((result.data.string.len) == (size_t)13u);
        check(memcmp(result.data.string.data, "scratch-owned", 13u) == 0);

        exprtk_value_destroy(&result);
        exprtk_env_free(&env);
    }

    it("moves and destroys promoted builtin results through canonical CMeta lifecycle") {
        const cmeta_data_desc *data = exprtk_value_cmeta_data();
        exprtk_env_t env;
        exprtk_value_t source;
        exprtk_value_t destination;

        exprtk_env_init(&env);
        source = exprtk_call_builtin(scratch_string_builtin, 0u, NULL, &env);

        check((source.type) == (EXPRTK_VAL_STRING));
        check((source.ownership) == (EXPRTK_VALUE_OWNED));
        check_equal(cmeta_data_value_init_zero(data, &destination), CMETA_OK);
        check((destination.type) == (EXPRTK_VAL_NULL));

        check_equal(
            cmeta_data_value_move(data, &destination, &source), CMETA_OK);
        check((source.type) == (EXPRTK_VAL_NULL));
        check((destination.type) == (EXPRTK_VAL_STRING));
        check((destination.ownership) == (EXPRTK_VALUE_OWNED));
        check_not_null(destination.storage);
        check((destination.data.string.len) == (size_t)13u);
        check(memcmp(
            destination.data.string.data, "scratch-owned", 13u) == 0);

        check_equal(
            cmeta_data_value_restore_zero(data, &destination), CMETA_OK);
        check((destination.type) == (EXPRTK_VAL_NULL));
        check_equal(
            cmeta_data_value_restore_zero(data, &destination), CMETA_OK);

        exprtk_env_free(&env);
    }

    it("fails closed for invalid module entries") {
        const exprtk_module_t *module = exprtk_module_math();
        exprtk_function_reflection_t reflected = {0};

        check_false(exprtk_module_function_reflect(
            module, module ? module->count : 0u, &reflected));
        check_false(exprtk_func_entry_reflect(NULL, &reflected));
        check_false(exprtk_func_entry_reflect(
            module ? &module->entries[0] : NULL, NULL));
    }
}
