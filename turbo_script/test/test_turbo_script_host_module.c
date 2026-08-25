#include "tinytest.h"

#include "turbo_script_host_api_internal.h"
#include "exprtk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

size_t ts_host_module_export_count(const turbo_script_module_t *module);
const char *ts_host_module_export_name(const turbo_script_module_t *module, size_t index);
uint32_t ts_host_module_export_arity(const turbo_script_module_t *module, size_t index);
size_t ts_host_module_parse_count(const turbo_script_module_t *module);
size_t ts_host_module_lower_count(const turbo_script_module_t *module);
const char *ts_host_module_source(const turbo_script_module_t *module);
const char *ts_host_module_name(const turbo_script_module_t *module);
turbo_script_status_t ts_host_module_execute_numeric(
    turbo_script_module_t *module, size_t export_index, int use_jit,
    const double *args, size_t arg_count, double *out_result);
turbo_script_status_t ts_host_module_execute_initializer(
    turbo_script_module_t *module, int use_jit);
turbo_script_status_t ts_host_module_compile_test_parse_oom(
    turbo_script_ctx_t *ctx, turbo_script_string_view_t source,
    const turbo_script_module_options_t *options, turbo_script_result_t *result,
    turbo_script_module_t **out_module);
turbo_script_status_t ts_host_module_test_ast_usage(
    exprtk_node_t *root, const turbo_script_module_options_t *options);

static turbo_script_status_t noop_host(void *user_data,
                                       const turbo_script_value_view_t *args,
                                       size_t arg_count,
                                       turbo_script_host_result_builder_t *builder) {
  turbo_script_value_view_t value = {.kind = TURBO_SCRIPT_VALUE_NUMBER};
  (void)user_data;
  (void)args;
  if (user_data) (*(size_t *)user_data)++;
  value.as.number = (double)arg_count;
  return turbo_script_host_result_set_value(builder, &value);
}

static turbo_script_status_t compile_text(turbo_script_ctx_t *ctx,
                                           turbo_script_result_t *result,
                                           const char *source,
                                           turbo_script_module_options_t *options,
                                           turbo_script_module_t **out_module) {
  turbo_script_string_view_t view = {source, strlen(source)};
  return turbo_script_module_compile(ctx, view, options, result, out_module);
}

static void destroy_fixture(turbo_script_ctx_t *ctx, turbo_script_result_t *result,
                            turbo_script_module_t *module) {
  if (module) check_equal(turbo_script_module_destroy(module, result),
                          TURBO_SCRIPT_STATUS_OK);
  turbo_script_result_destroy(result);
  turbo_script_free(ctx);
}

spec("TurboScript immutable Host modules") {
  it("copies source and module name and publishes ordered exports") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    char source[96] = "func add(a, b) { return a + b; };export(\"add\");";
    char name[16] = "math";
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    options.module_name = (turbo_script_string_view_t){name, strlen(name)};
    check_equal(compile_text(ctx, result, source, &options, &module),
                TURBO_SCRIPT_STATUS_OK);
    memset(source, 'x', strlen(source));
    memset(name, 'y', strlen(name));
    check_equal(ts_host_module_export_count(module), (size_t)1);
    check_equal(ts_host_module_export_name(module, 0), "add");
    check_equal(ts_host_module_export_arity(module, 0), (uint32_t)2);
    check_equal(ts_host_module_parse_count(module), (size_t)1);
    check_equal(ts_host_module_lower_count(module), (size_t)1);
    check_not_null(strstr(ts_host_module_source(module), "func add"));
    check_equal(ts_host_module_name(module), "math");
    destroy_fixture(ctx, result, module);
  }

  it("accepts a valid module with an empty export table") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    check_equal(compile_text(ctx, result, "value = 1;", &options, &module),
                TURBO_SCRIPT_STATUS_OK);
    check_equal(ts_host_module_export_count(module), (size_t)0);
    destroy_fixture(ctx, result, module);
  }

  it("rejects malformed and semantically invalid exports with compile errors") {
    static const char *const invalid[] = {
        "export(\"missing\");",
        "func f() { return 1; };export(\"f\");export(\"f\");",
        "name = \"f\";func f() { return 1; };export(name);",
        "func f() { return 1; };export(\"f\", 1);",
        "func f(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q) { return 1; };export(\"f\");"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_result_t *result = NULL;
      turbo_script_module_t *module = NULL;
      turbo_script_module_options_t options;
      check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
      turbo_script_module_options_init(&options);
      check_equal(compile_text(ctx, result, invalid[i], &options, &module),
                  TURBO_SCRIPT_STATUS_VALIDATION_ERROR);
      check_null(module);
      destroy_fixture(ctx, result, NULL);
    }
  }

  it("accepts exported arities zero one and sixteen") {
    const char *source =
        "func z(){return 0;};func o(a){return a;};"
        "func s(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p){return a;};"
        "export(\"z\");export(\"o\");export(\"s\");";
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    check_equal(compile_text(ctx, result, source, &options, &module), TURBO_SCRIPT_STATUS_OK);
    check_equal(ts_host_module_export_arity(module, 0), (uint32_t)0);
    check_equal(ts_host_module_export_arity(module, 1), (uint32_t)1);
    check_equal(ts_host_module_export_arity(module, 2), (uint32_t)16);
    destroy_fixture(ctx, result, module);
  }

  it("reports parse and export-validation source spans") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    turbo_script_error_info_t error = {.struct_size = sizeof(error)};
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    check_equal(compile_text(ctx, result, "value = 1;\nfunc broken(", &options, &module),
                TURBO_SCRIPT_STATUS_PARSE_ERROR);
    check_equal(turbo_script_result_get_error(result, &error), TURBO_SCRIPT_STATUS_OK);
    check_equal(error.phase, TURBO_SCRIPT_ERROR_PHASE_COMPILE);
    check_equal(error.line, (uint32_t)2);
    check_greater(error.column, (uint32_t)0);
    check_equal(compile_text(ctx, result,
                             "func f(){return 1;};\nexport(\"missing\");",
                             &options, &module), TURBO_SCRIPT_STATUS_VALIDATION_ERROR);
    error.struct_size = sizeof(error);
    check_equal(turbo_script_result_get_error(result, &error), TURBO_SCRIPT_STATUS_OK);
    check_equal(error.line, (uint32_t)2);
    check_greater(error.column, (uint32_t)0);
    destroy_fixture(ctx, result, NULL);
  }

  it("enforces source export and string quotas") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    const char *source = "func add(a,b){return a+b;};export(\"add\");";
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    options.max_source_bytes = strlen(source) - 1;
    check_equal(compile_text(ctx, result, source, &options, &module),
                TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
    turbo_script_module_options_init(&options);
    options.max_exports = 0;
    check_equal(compile_text(ctx, result, source, &options, &module),
                TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
    turbo_script_module_options_init(&options);
    options.max_string_bytes = 2;
    check_equal(compile_text(ctx, result, source, &options, &module),
                TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
    turbo_script_module_options_init(&options);
    options.max_ast_nodes = 1;
    check_equal(compile_text(ctx, result, "a = 1;", &options, &module),
                TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
    turbo_script_module_options_init(&options);
    options.max_imports = 0;
    check_equal(compile_text(ctx, result, "import(\"x\");", &options, &module),
                TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
    destroy_fixture(ctx, result, NULL);
  }

  it("leaves registry mutable after failure and freezes it until module destroy") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    turbo_script_host_function_descriptor_t descriptor = {0};
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    check_equal(compile_text(ctx, result, "export(\"missing\");", &options, &module),
                TURBO_SCRIPT_STATUS_VALIDATION_ERROR);
    descriptor.struct_size = sizeof(descriptor);
    descriptor.name = (turbo_script_string_view_t){"host_a", 6};
    check_equal(turbo_script_context_register_host_function(
                    ctx, &descriptor, noop_host, NULL, result), TURBO_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result, "func f(){return 1;};export(\"f\");",
                             &options, &module), TURBO_SCRIPT_STATUS_OK);
    descriptor.name = (turbo_script_string_view_t){"host_b", 6};
    check_equal(turbo_script_context_register_host_function(
                    ctx, &descriptor, noop_host, NULL, result),
                TURBO_SCRIPT_STATUS_INVALID_STATE);
    check_equal(turbo_script_module_destroy(module, result), TURBO_SCRIPT_STATUS_OK);
    module = NULL;
    check_equal(turbo_script_context_register_host_function(
                    ctx, &descriptor, noop_host, NULL, result), TURBO_SCRIPT_STATUS_OK);
    destroy_fixture(ctx, result, NULL);
  }

  it("resolves registered host callsites during compile") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    turbo_script_host_function_descriptor_t descriptor = {
        .struct_size = sizeof(descriptor), .min_arity = 1, .max_arity = 1,
        .name = {"host_count", 10}};
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    check_equal(turbo_script_context_register_host_function(
                    ctx, &descriptor, noop_host, NULL, result), TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    check_equal(compile_text(ctx, result,
                             "func f(a){return host_count(a);};export(\"f\");",
                             &options, &module), TURBO_SCRIPT_STATUS_OK);
    destroy_fixture(ctx, result, module);
  }

  it("executes one lowered artifact through interpreter then JIT") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    double args[2] = {20.0, 22.0};
    double interp = 0.0;
    double jit = 0.0;
    turbo_script_module_options_init(&options);
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func add(a,b){return a+b;};export(\"add\");",
                             &options, &module), TURBO_SCRIPT_STATUS_OK);
    check_equal(ts_host_module_execute_numeric(module, 0, 0, args, 2, &interp),
                TURBO_SCRIPT_STATUS_OK);
    check_equal(ts_host_module_execute_numeric(module, 0, 1, args, 2, &jit),
                TURBO_SCRIPT_STATUS_OK);
    check_equal(interp, 42.0);
    check_equal(jit, 42.0);
    check_equal(ts_host_module_lower_count(module), (size_t)1);
    destroy_fixture(ctx, result, module);
  }

  it("rejects under and over arity before touching MIR argument storage") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    double args[3] = {1, 2, 3};
    double output = 99;
    turbo_script_module_options_init(&options);
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func add(a,b){return a+b;};export(\"add\");",
                             &options, &module), TURBO_SCRIPT_STATUS_OK);
    for (int jit = 0; jit <= 1; ++jit) {
      output = 99;
      check_equal(ts_host_module_execute_numeric(module, 0, jit, args, 1, &output),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(output, 99.0);
      check_equal(ts_host_module_execute_numeric(module, 0, jit, args, 3, &output),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(output, 99.0);
    }
    destroy_fixture(ctx, result, module);
  }

  it("executes arity sixteen safely through interpreter and JIT") {
    const char *source =
        "func sum(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p){"
        "return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p;};export(\"sum\");";
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    double args[17];
    double output = 0;
    for (size_t i = 0; i < 17; ++i) args[i] = (double)(i + 1);
    turbo_script_module_options_init(&options);
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result, source, &options, &module), TURBO_SCRIPT_STATUS_OK);
    check_equal(ts_host_module_execute_numeric(module, 0, 0, args, 16, &output),
                TURBO_SCRIPT_STATUS_OK);
    check_equal(output, 136.0);
    check_equal(ts_host_module_execute_numeric(module, 0, 1, args, 16, &output),
                TURBO_SCRIPT_STATUS_OK);
    check_equal(output, 136.0);
    for (int jit = 0; jit <= 1; ++jit) {
      output = 99;
      check_equal(ts_host_module_execute_numeric(module, 0, jit, args, 17, &output),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(output, 99.0);
    }
    destroy_fixture(ctx, result, module);
  }

  it("executes registered host callback by frozen slot in both modes") {
    size_t callback_count = 0;
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    turbo_script_host_function_descriptor_t descriptor = {
        .struct_size = sizeof(descriptor), .min_arity = 1, .max_arity = 1,
        .name = {"host_count", 10}};
    double arg = 7;
    double output = 0;
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    check_equal(turbo_script_context_register_host_function(
                    ctx, &descriptor, noop_host, &callback_count, result),
                TURBO_SCRIPT_STATUS_OK);
    turbo_script_module_options_init(&options);
    check_equal(compile_text(ctx, result,
                             "func f(){return host_count();};export(\"f\");",
                             &options, &module),
                TURBO_SCRIPT_STATUS_UNSUPPORTED_BACKEND_SEMANTIC);
    check_null(module);
    check_equal(compile_text(ctx, result,
                             "func f(a){return host_count(a);};export(\"f\");",
                             &options, &module), TURBO_SCRIPT_STATUS_OK);
    descriptor.name = (turbo_script_string_view_t){"host_after_freeze", 17};
    check_equal(turbo_script_context_register_host_function(
                    ctx, &descriptor, noop_host, &callback_count, result),
                TURBO_SCRIPT_STATUS_INVALID_STATE);
    check_equal(ts_host_module_execute_numeric(module, 0, 0, &arg, 1, &output),
                TURBO_SCRIPT_STATUS_OK);
    check_equal(output, 1.0);
    check_equal(ts_host_module_execute_numeric(module, 0, 1, &arg, 1, &output),
                TURBO_SCRIPT_STATUS_OK);
    check_equal(output, 1.0);
    check_equal(callback_count, (size_t)2);
    destroy_fixture(ctx, result, module);
  }

  it("executes initializer in both modes without invoking export metadata") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    turbo_script_module_options_init(&options);
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func f(){return 1;};export(\"f\");",
                             &options, &module), TURBO_SCRIPT_STATUS_OK);
    check_equal(ts_host_module_execute_initializer(module, 0), TURBO_SCRIPT_STATUS_OK);
    check_equal(ts_host_module_execute_initializer(module, 1), TURBO_SCRIPT_STATUS_OK);
    destroy_fixture(ctx, result, module);
  }

  it("maps deterministic parser allocation failure to out of memory") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    turbo_script_result_t *result = NULL;
    turbo_script_module_t *module = NULL;
    turbo_script_module_options_t options;
    turbo_script_error_info_t error = {.struct_size = sizeof(error)};
    turbo_script_status_t status;
    turbo_script_module_options_init(&options);
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    status = ts_host_module_compile_test_parse_oom(
        ctx, (turbo_script_string_view_t){"a=1;", 4}, &options, result, &module);
    check_equal(status, TURBO_SCRIPT_STATUS_OUT_OF_MEMORY);
    check_null(module);
    check_equal(turbo_script_result_get_error(result, &error), TURBO_SCRIPT_STATUS_OK);
    check_equal(error.status, TURBO_SCRIPT_STATUS_OUT_OF_MEMORY);
    destroy_fixture(ctx, result, NULL);
  }

  it("counts class generator yield and template nodes at exact boundaries") {
    exprtk_node_t text = {.type = EXPRTK_NODE_TEMPLATE_STRING};
    exprtk_node_t yielded = {.type = EXPRTK_NODE_YIELD};
    exprtk_node_t generator = {.type = EXPRTK_NODE_GENERATOR_FUNCTION};
    exprtk_node_t method = {.type = EXPRTK_NODE_METHOD};
    exprtk_node_t *methods[1] = {&method};
    exprtk_node_t klass = {.type = EXPRTK_NODE_CLASS_DEF};
    turbo_script_module_options_t options;
    text.data.template_string.template_str = "four";
    text.data.template_string.len = 4;
    yielded.data.yield_expr.value = &text;
    generator.data.generator_def.body = &yielded;
    method.data.method.body = &generator;
    klass.data.class_def.methods = methods;
    klass.data.class_def.method_count = 1;
    turbo_script_module_options_init(&options);
    options.max_ast_nodes = 4;
    check_equal(ts_host_module_test_ast_usage(&klass, &options),
                TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
    options.max_ast_nodes = 5;
    options.max_string_bytes = 3;
    check_equal(ts_host_module_test_ast_usage(&klass, &options),
                TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
    options.max_string_bytes = 4;
    check_equal(ts_host_module_test_ast_usage(&klass, &options),
                TURBO_SCRIPT_STATUS_OK);
  }
}
