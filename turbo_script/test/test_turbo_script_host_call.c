#include "tinytest.h"

#include "../src/host/turbo_script_host_internal.h"
#include "../src/mir/turbo_script_mir_internal.h"
#include "../src/turbo_script_internal.h"
#include "turbo_script_host_api_internal.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static const char k_call_source[] =
    "counter=0;"
    "func echo(value){return value;};"
    "func zero(){return 7;};"
    "func sum16(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p){"
    "return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p;};"
    "func next(){counter=counter+1;return counter;};"
    "export(\"echo\");export(\"zero\");export(\"sum16\");export(\"next\");";

typedef struct host_call_fixture_s {
  turbo_script_ctx_t *ctx;
  turbo_script_result_t *result;
  turbo_script_module_t *module;
  turbo_script_instance_t *instance;
  turbo_script_call_options_t options;
} host_call_fixture_t;

static void host_call_fixture_open_with_node_limit(host_call_fixture_t *fixture,
                                                   turbo_script_execution_mode_t mode,
                                                   size_t max_value_nodes) {
  turbo_script_module_options_t module_options;
  turbo_script_instance_options_t instance_options;
  memset(fixture, 0, sizeof(*fixture));
  fixture->ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  check_not_null(fixture->ctx);
  check_equal(turbo_script_result_create(fixture->ctx, &fixture->result), TURBO_SCRIPT_STATUS_OK);
  turbo_script_module_options_init(&module_options);
  module_options.module_name = (turbo_script_string_view_t){"call-test", 9};
  check_equal(turbo_script_module_compile(
                  fixture->ctx,
                  (turbo_script_string_view_t){k_call_source, sizeof(k_call_source) - 1},
                  &module_options, fixture->result, &fixture->module),
              TURBO_SCRIPT_STATUS_OK);
  turbo_script_instance_options_init(&instance_options);
  instance_options.mode = mode;
  instance_options.max_value_nodes = max_value_nodes;
  check_equal(turbo_script_instance_create(fixture->module, &instance_options, fixture->result,
                                           &fixture->instance),
              TURBO_SCRIPT_STATUS_OK);
  turbo_script_call_options_init(&fixture->options);
}

static void host_call_fixture_open(host_call_fixture_t *fixture,
                                   turbo_script_execution_mode_t mode) {
  host_call_fixture_open_with_node_limit(fixture, mode, 65536);
}

static void host_call_fixture_close(host_call_fixture_t *fixture) {
  if (fixture->instance)
    check_equal(turbo_script_instance_destroy(fixture->instance, fixture->result),
                TURBO_SCRIPT_STATUS_OK);
  if (fixture->module)
    check_equal(turbo_script_module_destroy(fixture->module, fixture->result),
                TURBO_SCRIPT_STATUS_OK);
  if (fixture->result) turbo_script_result_destroy(fixture->result);
  if (fixture->ctx) turbo_script_free(fixture->ctx);
}

static turbo_script_export_handle_t host_call_resolve(host_call_fixture_t *fixture,
                                                      const char *name) {
  turbo_script_export_handle_t handle = 0;
  check_equal(turbo_script_instance_resolve_export(fixture->instance,
                                                   (turbo_script_string_view_t){name, strlen(name)},
                                                   fixture->result, &handle),
              TURBO_SCRIPT_STATUS_OK);
  check(handle != 0);
  return handle;
}

static turbo_script_value_view_t host_call(host_call_fixture_t *fixture,
                                           turbo_script_export_handle_t handle,
                                           const turbo_script_value_view_t *args,
                                           size_t arg_count) {
  turbo_script_value_view_t actual = {0};
  check_equal(turbo_script_instance_call(fixture->instance, handle, args, arg_count,
                                         &fixture->options, fixture->result),
              TURBO_SCRIPT_STATUS_OK);
  check_equal(turbo_script_result_get_value(fixture->result, &actual), TURBO_SCRIPT_STATUS_OK);
  return actual;
}

static int number_equal(double lhs, double rhs) {
  double difference;
  double scale;
  if (isnan(lhs) || isnan(rhs)) return isnan(lhs) && isnan(rhs);
  if (isinf(lhs) || isinf(rhs)) return lhs == rhs;
  difference = fabs(lhs - rhs);
  scale = fmax(fmax(fabs(lhs), fabs(rhs)), 1.0);
  return difference <= 1e-12 || difference <= scale * 1e-12;
}

static void check_value_equal(const turbo_script_value_view_t *actual,
                              const turbo_script_value_view_t *expected) {
  check_equal(actual->kind, expected->kind);
  if (actual->kind != expected->kind) return;
  switch (expected->kind) {
  case TURBO_SCRIPT_VALUE_NULL:
    break;
  case TURBO_SCRIPT_VALUE_BOOL:
    check_equal(actual->as.boolean, expected->as.boolean);
    break;
  case TURBO_SCRIPT_VALUE_INT64:
    check_equal(actual->as.integer, expected->as.integer);
    break;
  case TURBO_SCRIPT_VALUE_NUMBER:
    check(number_equal(actual->as.number, expected->as.number));
    break;
  case TURBO_SCRIPT_VALUE_STRING:
    check_equal(actual->as.string.size, expected->as.string.size);
    if (actual->as.string.size == expected->as.string.size)
      check(memcmp(actual->as.string.data, expected->as.string.data, expected->as.string.size) ==
            0);
    break;
  case TURBO_SCRIPT_VALUE_ARRAY:
    check_equal(actual->as.array.count, expected->as.array.count);
    if (actual->as.array.count == expected->as.array.count)
      for (size_t i = 0; i < expected->as.array.count; ++i)
        check_value_equal(&actual->as.array.items[i], &expected->as.array.items[i]);
    break;
  case TURBO_SCRIPT_VALUE_RECORD:
    check_equal(actual->as.record.count, expected->as.record.count);
    if (actual->as.record.count == expected->as.record.count) {
      for (size_t i = 0; i < expected->as.record.count; ++i) {
        const turbo_script_record_entry_view_t *match = NULL;
        for (size_t j = 0; j < actual->as.record.count; ++j) {
          if (actual->as.record.entries[j].key.size == expected->as.record.entries[i].key.size &&
              memcmp(actual->as.record.entries[j].key.data, expected->as.record.entries[i].key.data,
                     expected->as.record.entries[i].key.size) == 0) {
            match = &actual->as.record.entries[j];
            break;
          }
        }
        check_not_null(match);
        if (match) check_value_equal(&match->value, &expected->as.record.entries[i].value);
      }
    }
    break;
  default:
    check(0);
    break;
  }
}

static int value_equals_number(const turbo_script_value_view_t *value, double expected) {
  if (value->kind == TURBO_SCRIPT_VALUE_NUMBER) return number_equal(value->as.number, expected);
  if (value->kind == TURBO_SCRIPT_VALUE_INT64)
    return number_equal((double)value->as.integer, expected);
  return 0;
}

static void run_value_matrix(turbo_script_execution_mode_t mode) {
  static const turbo_script_value_view_t nested_items[] = {
      {.kind = TURBO_SCRIPT_VALUE_NULL},
      {.kind = TURBO_SCRIPT_VALUE_BOOL, .as.boolean = 1},
      {.kind = TURBO_SCRIPT_VALUE_STRING, .as.string = {"\xE4\xB8\xAD", 3}}};
  static const turbo_script_record_entry_view_t nested_entries[] = {
      {{"items", 5},
       {.kind = TURBO_SCRIPT_VALUE_ARRAY,
        .as.array = {nested_items, sizeof(nested_items) / sizeof(nested_items[0])}}},
      {{"count", 5}, {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = INT64_MAX}}};
  static const turbo_script_value_view_t cases[] = {
      {.kind = TURBO_SCRIPT_VALUE_NULL},
      {.kind = TURBO_SCRIPT_VALUE_BOOL, .as.boolean = 1},
      {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = INT64_MIN},
      {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = INT64_MAX},
      {.kind = TURBO_SCRIPT_VALUE_NUMBER, .as.number = 3.141592653589793},
      {.kind = TURBO_SCRIPT_VALUE_STRING, .as.string = {"TurboScript \xE2\x9C\x93", 15}},
      {.kind = TURBO_SCRIPT_VALUE_ARRAY, .as.array = {NULL, 0}},
      {.kind = TURBO_SCRIPT_VALUE_RECORD, .as.record = {NULL, 0}},
      {.kind = TURBO_SCRIPT_VALUE_RECORD,
       .as.record = {nested_entries, sizeof(nested_entries) / sizeof(nested_entries[0])}},
      {.kind = TURBO_SCRIPT_VALUE_NUMBER, .as.number = INFINITY},
      {.kind = TURBO_SCRIPT_VALUE_NUMBER, .as.number = -INFINITY},
      {.kind = TURBO_SCRIPT_VALUE_NUMBER, .as.number = NAN}};
  host_call_fixture_t fixture;
  turbo_script_export_handle_t echo;
  host_call_fixture_open(&fixture, mode);
  echo = host_call_resolve(&fixture, "echo");
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    turbo_script_value_view_t actual = host_call(&fixture, echo, &cases[i], 1);
    check_value_equal(&actual, &cases[i]);
  }
  host_call_fixture_close(&fixture);
}

static void run_arity_and_state_matrix(turbo_script_execution_mode_t mode) {
  host_call_fixture_t fixture;
  turbo_script_export_handle_t zero;
  turbo_script_export_handle_t sum16;
  turbo_script_export_handle_t next;
  turbo_script_value_view_t args[16];
  turbo_script_value_view_t actual;
  turbo_script_error_info_t error;
  host_call_fixture_open(&fixture, mode);
  zero = host_call_resolve(&fixture, "zero");
  sum16 = host_call_resolve(&fixture, "sum16");
  next = host_call_resolve(&fixture, "next");

  actual = host_call(&fixture, zero, NULL, 0);
  check(value_equals_number(&actual, 7.0));
  for (size_t i = 0; i < 16; ++i) {
    args[i].kind = TURBO_SCRIPT_VALUE_NUMBER;
    args[i].reserved = 0;
    args[i].as.number = (double)(i + 1);
  }
  actual = host_call(&fixture, sum16, args, 16);
  check_equal(actual.kind, TURBO_SCRIPT_VALUE_NUMBER);
  check(number_equal(actual.as.number, 136.0));

  check_equal(turbo_script_instance_call(fixture.instance, sum16, args, 15, &fixture.options,
                                         fixture.result),
              TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
  check_equal(turbo_script_result_get_value(fixture.result, &actual),
              TURBO_SCRIPT_STATUS_INVALID_STATE);
  memset(&error, 0, sizeof(error));
  error.struct_size = sizeof(error);
  check_equal(turbo_script_result_get_error(fixture.result, &error), TURBO_SCRIPT_STATUS_OK);
  check_equal(error.phase, TURBO_SCRIPT_ERROR_PHASE_CALL);

  actual = host_call(&fixture, next, NULL, 0);
  check(value_equals_number(&actual, 1.0));
  actual = host_call(&fixture, next, NULL, 0);
  check(value_equals_number(&actual, 2.0));
  host_call_fixture_close(&fixture);
}

static void run_instance_isolation(turbo_script_execution_mode_t mode) {
  host_call_fixture_t first;
  host_call_fixture_t second;
  turbo_script_value_view_t actual;
  turbo_script_export_handle_t first_next;
  turbo_script_export_handle_t second_next;
  host_call_fixture_open(&first, mode);
  host_call_fixture_open(&second, mode);
  first_next = host_call_resolve(&first, "next");
  second_next = host_call_resolve(&second, "next");
  actual = host_call(&first, first_next, NULL, 0);
  check(value_equals_number(&actual, 1.0));
  actual = host_call(&first, first_next, NULL, 0);
  check(value_equals_number(&actual, 2.0));
  actual = host_call(&second, second_next, NULL, 0);
  check(value_equals_number(&actual, 1.0));
  host_call_fixture_close(&second);
  host_call_fixture_close(&first);
}

static void run_aggregate_argument_limit(turbo_script_execution_mode_t mode) {
  host_call_fixture_t fixture;
  turbo_script_export_handle_t sum16;
  turbo_script_value_view_t args[16] = {0};
  turbo_script_value_view_t value = {0};
  turbo_script_error_info_t error = {.struct_size = sizeof(error)};
  host_call_fixture_open_with_node_limit(&fixture, mode, 15);
  sum16 = host_call_resolve(&fixture, "sum16");
  for (size_t i = 0; i < 16; ++i) {
    args[i].kind = TURBO_SCRIPT_VALUE_NUMBER;
    args[i].as.number = (double)i;
  }
  check_equal(turbo_script_instance_call(fixture.instance, sum16, args, 16, &fixture.options,
                                         fixture.result),
              TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_equal(turbo_script_result_get_value(fixture.result, &value),
              TURBO_SCRIPT_STATUS_INVALID_STATE);
  check_equal(turbo_script_result_get_error(fixture.result, &error), TURBO_SCRIPT_STATUS_OK);
  check_equal(error.phase, TURBO_SCRIPT_ERROR_PHASE_CALL);
  host_call_fixture_close(&fixture);
}

static void run_resolved_slot_without_name_lookup(turbo_script_execution_mode_t mode) {
  host_call_fixture_t fixture;
  turbo_script_export_handle_t echo;
  turbo_script_value_view_t expected = {.kind = TURBO_SCRIPT_VALUE_STRING,
                                        .as.string = {"bound", 5}};
  turbo_script_value_view_t actual;
  exprtk_func_t *function = NULL;
  char *saved_name;
  host_call_fixture_open(&fixture, mode);
  echo = host_call_resolve(&fixture, "echo");
  for (exprtk_func_t *candidate = fixture.instance->runtime_ctx->env.funcs; candidate;
       candidate = candidate->next) {
    if (candidate->name && strcmp(candidate->name, "echo") == 0) {
      function = candidate;
      break;
    }
  }
  check_not_null(function);
  if (!function) {
    host_call_fixture_close(&fixture);
    return;
  }
  saved_name = function->name;
  function->name = "renamed-after-resolve";
  actual = host_call(&fixture, echo, &expected, 1);
  function->name = saved_name;
  check_value_equal(&actual, &expected);
  host_call_fixture_close(&fixture);
}

spec("TurboScript generic Host export calls") {
  it("marks only type-stable number exports for native MIR calls") {
    host_call_fixture_t fixture;
    host_call_fixture_open(&fixture, TURBO_SCRIPT_EXEC_JIT);
    check(ts_mir_artifact_export_is_native_numeric(fixture.module->artifact, 0));
    check(!ts_mir_artifact_export_is_native_numeric(fixture.module->artifact, 1));
    check(ts_mir_artifact_export_is_native_numeric(fixture.module->artifact, 2));
    check(!ts_mir_artifact_export_is_native_numeric(fixture.module->artifact, 3));
    host_call_fixture_close(&fixture);
  }
  it("uses the bounded call profile defaults") {
    turbo_script_call_options_t options;
    turbo_script_call_options_init(&options);
    check_equal(options.struct_size, sizeof(options));
    check_equal(options.max_recursion, (uint32_t)100);
    check_equal(options.max_steps, (uint32_t)100000);
    check_equal(options.max_loop_iterations, (uint32_t)10000);
    check_equal(options.max_host_callbacks, (uint32_t)1024);
    check_equal(options.max_result_bytes, (size_t)(4 * 1024 * 1024));
  }
  it("preserves every public value kind in the interpreter") {
    run_value_matrix(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("preserves every public value kind in the JIT") { run_value_matrix(TURBO_SCRIPT_EXEC_JIT); }
  it("enforces arity and preserves state in the interpreter") {
    run_arity_and_state_matrix(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("enforces arity and preserves state in the JIT") {
    run_arity_and_state_matrix(TURBO_SCRIPT_EXEC_JIT);
  }
  it("isolates closure state between interpreter instances") {
    run_instance_isolation(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("isolates closure state between JIT instances") {
    run_instance_isolation(TURBO_SCRIPT_EXEC_JIT);
  }
  it("enforces aggregate argument quotas in the interpreter") {
    run_aggregate_argument_limit(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("enforces aggregate argument quotas in the JIT") {
    run_aggregate_argument_limit(TURBO_SCRIPT_EXEC_JIT);
  }
  it("calls a resolved interpreter slot without a name lookup") {
    run_resolved_slot_without_name_lookup(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("calls a resolved JIT slot without a name lookup") {
    run_resolved_slot_without_name_lookup(TURBO_SCRIPT_EXEC_JIT);
  }
}
