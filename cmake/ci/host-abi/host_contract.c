/* Installed C11/C++17 consumer: every Host ABI v1 entry point is linked and used. */
#include <turbo_script.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
#define ABI_ASSERT static_assert
#else
#define ABI_ASSERT _Static_assert
#endif
ABI_ASSERT(TURBO_SCRIPT_HOST_ABI_VERSION == 1u, "Host ABI version");
ABI_ASSERT(sizeof(turbo_script_status_t) == 4, "Fixed-width status");
ABI_ASSERT(sizeof(turbo_script_export_handle_t) == 8, "Fixed-width handle");
ABI_ASSERT(offsetof(turbo_script_module_options_t, struct_size) == 0, "Module prefix");
ABI_ASSERT(offsetof(turbo_script_instance_options_t, struct_size) == 0, "Instance prefix");
ABI_ASSERT(offsetof(turbo_script_call_options_t, struct_size) == 0, "Call prefix");

static turbo_script_string_view_t view(const char *text) {
  turbo_script_string_view_t value;
  value.data = text;
  value.size = strlen(text);
  return value;
}

static turbo_script_status_t echo_host(void *user, const turbo_script_value_view_t *args,
                                       size_t count, turbo_script_host_result_builder_t *builder) {
  unsigned *calls = (unsigned *)user;
  if (count != 1) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  ++*calls;
  return turbo_script_host_result_set_value(builder, &args[0]);
}

static turbo_script_status_t fail_host(void *user, const turbo_script_value_view_t *args,
                                       size_t count, turbo_script_host_result_builder_t *builder) {
  (void)user;
  (void)args;
  if (count != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  return turbo_script_host_result_set_error(builder, -77, view("installed host failure"));
}

static int is_number(const turbo_script_value_view_t *value, int expected) {
  if (value->kind == TURBO_SCRIPT_VALUE_INT64) return value->as.integer == expected;
  return value->kind == TURBO_SCRIPT_VALUE_NUMBER && value->as.number == (double)expected;
}

#define REQUIRE(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "Host ABI contract failed at line %d: %s\n", __LINE__, #condition); \
    failed = 1; goto cleanup; \
  } \
} while (0)

static int exercise(turbo_script_execution_mode_t mode) {
  static const char source[] =
      "counter=0;func next(){counter=counter+1;return counter;};"
      "func echo(value){return value;};"
      "func callback(value){return abi_echo(value);};"
      "func host_failure(){return abi_fail();};"
      "export(\"next\");export(\"echo\");export(\"callback\");export(\"host_failure\");";
  turbo_script_ctx_t *ctx = NULL;
  turbo_script_module_t *module = NULL;
  turbo_script_instance_t *one = NULL, *two = NULL;
  turbo_script_result_t *result = NULL;
  turbo_script_module_options_t module_options;
  turbo_script_instance_options_t instance_options;
  turbo_script_call_options_t call_options;
  turbo_script_export_info_t info;
  turbo_script_error_info_t error;
  turbo_script_host_function_descriptor_t descriptor;
  turbo_script_value_view_t input, output;
  turbo_script_export_handle_t next_one = 0, next_two = 0, echo = 0, callback = 0, failure = 0;
  turbo_script_status_t status;
  char text[] = {'A', '\0', (char)0xe4, (char)0xb8, (char)0xad};
  unsigned callback_count = 0;
  int failed = 0;

  ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
  REQUIRE(ctx != NULL);
  REQUIRE(turbo_script_result_create(ctx, &result) == TURBO_SCRIPT_STATUS_OK);
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.struct_size = sizeof(descriptor);
  descriptor.name = view("abi_echo");
  descriptor.min_arity = descriptor.max_arity = 1;
  REQUIRE(turbo_script_context_register_host_function(ctx, &descriptor, echo_host,
                                                      &callback_count, result) == TURBO_SCRIPT_STATUS_OK);
  descriptor.name = view("abi_fail");
  descriptor.min_arity = descriptor.max_arity = 0;
  REQUIRE(turbo_script_context_register_host_function(ctx, &descriptor, fail_host,
                                                      NULL, result) == TURBO_SCRIPT_STATUS_OK);
  turbo_script_module_options_init(&module_options);
  module_options.module_name = view("installed-host-contract");
  REQUIRE(turbo_script_module_compile(ctx, view(source), &module_options, result, &module) == TURBO_SCRIPT_STATUS_OK);
  turbo_script_instance_options_init(&instance_options);
  instance_options.mode = mode;
  REQUIRE(turbo_script_instance_create(module, &instance_options, result, &one) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(turbo_script_instance_create(module, &instance_options, result, &two) == TURBO_SCRIPT_STATUS_OK);
  turbo_script_call_options_init(&call_options);
  REQUIRE(turbo_script_instance_resolve_export(one, view("next"), result, &next_one) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(turbo_script_instance_resolve_export(two, view("next"), result, &next_two) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(next_one != 0 && next_two != 0 && next_one != next_two);
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  REQUIRE(turbo_script_instance_get_export_info(one, next_one, result, &info) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(info.min_arity == 0 && info.max_arity == 0);
  REQUIRE(info.name.size == 4 && memcmp(info.name.data, "next", 4) == 0);
  REQUIRE(turbo_script_instance_call(one, next_one, NULL, 0, &call_options, result) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(turbo_script_result_get_value(result, &output) == TURBO_SCRIPT_STATUS_OK && is_number(&output, 1));
  REQUIRE(turbo_script_instance_call(one, next_one, NULL, 0, &call_options, result) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(turbo_script_result_get_value(result, &output) == TURBO_SCRIPT_STATUS_OK && is_number(&output, 2));
  REQUIRE(turbo_script_instance_call(two, next_two, NULL, 0, &call_options, result) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(turbo_script_result_get_value(result, &output) == TURBO_SCRIPT_STATUS_OK && is_number(&output, 1));
  REQUIRE(turbo_script_instance_call(two, next_one, NULL, 0, &call_options, result) == TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE);
  REQUIRE(turbo_script_result_get_value(result, &output) == TURBO_SCRIPT_STATUS_INVALID_STATE);
  turbo_script_result_reset(result);

  REQUIRE(turbo_script_instance_resolve_export(one, view("echo"), result, &echo) == TURBO_SCRIPT_STATUS_OK);
  memset(&input, 0, sizeof(input));
  input.kind = TURBO_SCRIPT_VALUE_STRING;
  input.as.string.data = text;
  input.as.string.size = sizeof(text);
  REQUIRE(turbo_script_instance_call(one, echo, &input, 1, &call_options, result) == TURBO_SCRIPT_STATUS_OK);
  text[0] = 'Z';
  REQUIRE(turbo_script_result_get_value(result, &output) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(output.kind == TURBO_SCRIPT_VALUE_STRING && output.as.string.size == sizeof(text));
  REQUIRE(output.as.string.data[0] == 'A' && output.as.string.data[1] == '\0');
  REQUIRE((unsigned char)output.as.string.data[2] == 0xe4);

  REQUIRE(turbo_script_instance_resolve_export(one, view("callback"), result, &callback) == TURBO_SCRIPT_STATUS_OK);
  memset(&input, 0, sizeof(input));
  input.kind = TURBO_SCRIPT_VALUE_NUMBER;
  input.as.number = 42;
  REQUIRE(turbo_script_instance_call(one, callback, &input, 1, &call_options, result) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(turbo_script_result_get_value(result, &output) == TURBO_SCRIPT_STATUS_OK && is_number(&output, 42));
  REQUIRE(callback_count == 1);
  REQUIRE(turbo_script_instance_resolve_export(one, view("host_failure"), result, &failure) == TURBO_SCRIPT_STATUS_OK);
  status = turbo_script_instance_call(one, failure, NULL, 0, &call_options, result);
  REQUIRE(status == TURBO_SCRIPT_STATUS_HOST_ERROR);
  memset(&error, 0, sizeof(error));
  error.struct_size = sizeof(error);
  REQUIRE(turbo_script_result_get_error(result, &error) == TURBO_SCRIPT_STATUS_OK);
  REQUIRE(error.status == TURBO_SCRIPT_STATUS_HOST_ERROR && error.cause_code == -77);
  REQUIRE(error.phase == TURBO_SCRIPT_ERROR_PHASE_HOST_CALLBACK);

cleanup:
  if (one && turbo_script_instance_destroy(one, result) != TURBO_SCRIPT_STATUS_OK) failed = 1;
  if (two && turbo_script_instance_destroy(two, result) != TURBO_SCRIPT_STATUS_OK) failed = 1;
  if (module && turbo_script_module_destroy(module, result) != TURBO_SCRIPT_STATUS_OK) failed = 1;
  if (ctx && result) {
    if (turbo_script_context_unregister_host_function(ctx, view("abi_echo"), result) != TURBO_SCRIPT_STATUS_OK) failed = 1;
    if (turbo_script_context_unregister_host_function(ctx, view("abi_fail"), result) != TURBO_SCRIPT_STATUS_OK) failed = 1;
  }
  if (result) turbo_script_result_destroy(result);
  if (ctx) turbo_script_free(ctx);
  if (!failed) printf("Installed Host ABI v1 passed: mode=%u; two isolated instances; owned values; callbacks and errors\n", (unsigned)mode);
  return failed;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  if (strcmp(argv[1], "interpreter") == 0) return exercise(TURBO_SCRIPT_EXEC_INTERPRETER);
  if (strcmp(argv[1], "jit") == 0) return exercise(TURBO_SCRIPT_EXEC_JIT);
  return 2;
}
