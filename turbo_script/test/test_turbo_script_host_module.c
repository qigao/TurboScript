#include "tinytest.h"

#include "turbo_script_host_api_internal.h"

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

static turbo_script_status_t noop_host(void *user_data,
                                       const turbo_script_value_view_t *args,
                                       size_t arg_count,
                                       turbo_script_host_result_builder_t *builder) {
  turbo_script_value_view_t value = {.kind = TURBO_SCRIPT_VALUE_NUMBER};
  (void)user_data;
  (void)args;
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
}
