#include "mapper.h"
#include "exprtk.h"
#include "tinytest.h"

#include <string.h>

static exprtk_value_t eval_script(exprtk_env_t *env, const char *script) {
    exprtk_node_t *root = exprtk_parse(script, strlen(script));
    exprtk_value_t result;
    check_not_null(root);
    result = exprtk_eval(root, env);
    exprtk_free(root);
    return result;
}

spec("mapper_module") {
    it("maps typed classes through JSON and writes them back") {
        exprtk_env_t env;
        exprtk_value_t klass;
        exprtk_value_t args[2];
        exprtk_value_t object;
        exprtk_value_t text;
        exprtk_value_t definition_result;
        void *module;

        exprtk_env_init(&env);
        module = mapper_ctx_create();
        check_not_null(module);
        mapper_load(module, &env, NULL);
        definition_result = eval_script(&env,
            "class User { name: string; age: int64; active: bool; };");
        exprtk_value_destroy(&definition_result);
        klass = exprtk_env_get(&env, "User");
        check((klass.type) == (EXPRTK_VAL_CLASS));
        args[0] = klass;
        args[1] = exprtk_val_str(vstr_from_cstr(
            "{\"name\":\"Ada\",\"age\":37,\"active\":true}"));
        object = exprtk_call_internal("mapper.read_json", 2, args, &env);
        check((object.type) == (EXPRTK_VAL_INSTANCE));
        args[0] = object;
        text = exprtk_call_internal("mapper.write_json", 1, args, &env);
        check((text.type) == (EXPRTK_VAL_STRING));
        check_contains(text.data.string.data, "Ada");
        check_contains(text.data.string.data, "37");
        exprtk_value_destroy(&text);
        exprtk_value_destroy(&object);
        mapper_ctx_destroy(module);
        exprtk_env_free(&env);
    }

    it("rejects a value with the wrong declared type") {
        exprtk_env_t env;
        exprtk_value_t klass;
        exprtk_value_t args[2];
        exprtk_value_t result;
        exprtk_value_t definition_result;
        void *module;

        exprtk_env_init(&env);
        module = mapper_ctx_create();
        mapper_load(module, &env, NULL);
        definition_result = eval_script(&env, "class User { age: int64; };");
        exprtk_value_destroy(&definition_result);
        klass = exprtk_env_get(&env, "User");
        args[0] = klass;
        args[1] = exprtk_val_str(vstr_from_cstr("{\"age\":\"bad\"}"));
        result = exprtk_call_internal("mapper.read_json", 2, args, &env);
        check((result.type) == (EXPRTK_VAL_NULL));
        check_contains(env.error_msg, "received string");
        mapper_ctx_destroy(module);
        exprtk_env_free(&env);
    }

    it("maps YAML and XML class documents") {
        exprtk_env_t env;
        exprtk_value_t klass;
        exprtk_value_t args[2];
        exprtk_value_t object;
        exprtk_value_t text;
        exprtk_value_t definition_result;
        void *module;

        exprtk_env_init(&env);
        module = mapper_ctx_create();
        check_not_null(module);
        mapper_load(module, &env, NULL);
        definition_result = eval_script(&env,
            "class User { name: string; age: int64; active: bool; };" );
        exprtk_value_destroy(&definition_result);
        klass = exprtk_env_get(&env, "User");
        check((klass.type) == (EXPRTK_VAL_CLASS));

        args[0] = klass;
        args[1] = exprtk_val_str(vstr_from_cstr(
            "name: Ada\nage: 37\nactive: true\n"));
        object = exprtk_call_internal("mapper.read_yaml", 2, args, &env);
        check((object.type) == (EXPRTK_VAL_INSTANCE));
        args[0] = object;
        text = exprtk_call_internal("mapper.write_xml", 1, args, &env);
        check((text.type) == (EXPRTK_VAL_STRING));
        check_contains(text.data.string.data, "<User>");
        check_contains(text.data.string.data, "<name>Ada</name>");
        exprtk_value_destroy(&text);
        exprtk_value_destroy(&object);

        args[0] = klass;
        args[1] = exprtk_val_str(vstr_from_cstr(
            "<User><name>Ada</name><age>37</age><active>true</active></User>"));
        object = exprtk_call_internal("mapper.read_xml", 2, args, &env);
        check((object.type) == (EXPRTK_VAL_INSTANCE));
        exprtk_value_destroy(&object);
        mapper_ctx_destroy(module);
        exprtk_env_free(&env);
    }

    it("enforces declared types on static class fields") {
        exprtk_env_t env;
        exprtk_value_t result;

        exprtk_env_init(&env);
        result = eval_script(&env,
            "class Settings { static retries: int64 = 3; };"
            "Settings.retries = \"invalid\";");
        check((env.flow) == (exprtk_FLOW_THROW));
        check_contains(env.error_msg, "static field 'retries'");
        exprtk_value_destroy(&result);
        exprtk_env_free(&env);
    }
}
