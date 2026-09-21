#include "tinytest.h"

#include "../src/host/turbo_script_host_internal.h"
#include "turbo_script.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

enum {
  TEST_DEFAULT_RECURSION_LIMIT = 100,
  TEST_DEFAULT_LOOP_LIMIT = 10000,
  TEST_DEFAULT_CALLBACK_LIMIT = 1024,
  TEST_RETAINED_LIMIT = 128 * 1024,
};

static const char k_limits_source[] =
    "blob=\"\";"
    "func simple(){return 1.5+2.5;};"
    "func loop(n){i=0;while(i<n){i=i+1;};return i;};"
    "func recurse(n){if(n<=0){return 0;};return recurse(n-1)+1;};"
    "func callbacks(n){i=0;total=0;while(i<n){total=total+host_tick();i=i+1;};"
    "return total;};"
    "func callback_once(){return host_tick();};"
    "func large(){return \"large\";};"
    "func grow(n){i=0;while(i<n){"
    "blob=blob+\"01234567890123456789012345678901234567890123456789"
    "01234567890123456789012345678901234567890123456789\";"
    "i=i+1;};return 1.5;};"
    "export(\"simple\");export(\"loop\");export(\"recurse\");"
    "export(\"callbacks\");export(\"callback_once\");export(\"large\");export(\"grow\");";

typedef struct interrupt_probe_s {
  atomic_int requested;
  size_t checks;
  size_t request_after;
} interrupt_probe_t;

typedef struct host_limits_fixture_s {
  turbo_script_ctx_t *ctx;
  turbo_script_result_t *result;
  turbo_script_module_t *module;
  turbo_script_instance_t *instance;
  turbo_script_call_options_t options;
  size_t host_calls;
} host_limits_fixture_t;

static turbo_script_status_t host_tick(void *user_data, const turbo_script_value_view_t *args,
                                       size_t arg_count,
                                       turbo_script_host_result_builder_t *builder) {
  host_limits_fixture_t *fixture = (host_limits_fixture_t *)user_data;
  turbo_script_value_view_t value = {
      .kind = TURBO_SCRIPT_VALUE_INT64,
      .as.integer = 1,
  };
  (void)args;
  (void)arg_count;
  fixture->host_calls++;
  return turbo_script_host_result_set_value(builder, &value);
}

static int test_interrupt(void *user_data) {
  interrupt_probe_t *probe = (interrupt_probe_t *)user_data;
  probe->checks++;
  if (probe->checks >= probe->request_after)
    atomic_store_explicit(&probe->requested, 1, memory_order_relaxed);
  return atomic_load_explicit(&probe->requested, memory_order_relaxed) != 0;
}

static void host_limits_fixture_open_with_recursion(host_limits_fixture_t *fixture,
                                                    turbo_script_execution_mode_t mode,
                                                    size_t max_stack_bytes,
                                                    size_t max_retained_bytes,
                                                    uint32_t max_recursion) {
  turbo_script_module_options_t module_options;
  turbo_script_instance_options_t instance_options;
  turbo_script_host_function_descriptor_t descriptor = {
      .struct_size = sizeof(descriptor),
      .min_arity = 0,
      .max_arity = 0,
      .name = {"host_tick", 9},
  };
  memset(fixture, 0, sizeof(*fixture));
  fixture->ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  check_not_null(fixture->ctx);
  check_equal(turbo_script_result_create(fixture->ctx, &fixture->result), TURBO_SCRIPT_STATUS_OK);
  check_equal(turbo_script_context_register_host_function(fixture->ctx, &descriptor, host_tick,
                                                          fixture, fixture->result),
              TURBO_SCRIPT_STATUS_OK);
  turbo_script_module_options_init(&module_options);
  module_options.module_name = (turbo_script_string_view_t){"limits-test", 11};
  check_equal(turbo_script_module_compile(
                  fixture->ctx,
                  (turbo_script_string_view_t){k_limits_source, sizeof(k_limits_source) - 1},
                  &module_options, fixture->result, &fixture->module),
              TURBO_SCRIPT_STATUS_OK);
  turbo_script_instance_options_init(&instance_options);
  instance_options.mode = mode;
  if (max_stack_bytes != 0) instance_options.max_stack_bytes = max_stack_bytes;
  if (max_retained_bytes != 0) instance_options.max_retained_bytes = max_retained_bytes;
  if (max_recursion != 0) instance_options.max_recursion = max_recursion;
  check_equal(turbo_script_instance_create(fixture->module, &instance_options, fixture->result,
                                           &fixture->instance),
              TURBO_SCRIPT_STATUS_OK);
  turbo_script_call_options_init(&fixture->options);
}

static void host_limits_fixture_open(host_limits_fixture_t *fixture,
                                     turbo_script_execution_mode_t mode, size_t max_stack_bytes,
                                     size_t max_retained_bytes) {
  host_limits_fixture_open_with_recursion(fixture, mode, max_stack_bytes, max_retained_bytes, 0);
}

static void host_limits_fixture_close(host_limits_fixture_t *fixture) {
  if (fixture->instance)
    check_equal(turbo_script_instance_destroy(fixture->instance, fixture->result),
                TURBO_SCRIPT_STATUS_OK);
  if (fixture->module)
    check_equal(turbo_script_module_destroy(fixture->module, fixture->result),
                TURBO_SCRIPT_STATUS_OK);
  if (fixture->result) turbo_script_result_destroy(fixture->result);
  if (fixture->ctx) turbo_script_free(fixture->ctx);
}

static turbo_script_export_handle_t host_limits_resolve(host_limits_fixture_t *fixture,
                                                        const char *name) {
  turbo_script_export_handle_t handle = 0;
  check_equal(turbo_script_instance_resolve_export(fixture->instance,
                                                   (turbo_script_string_view_t){name, strlen(name)},
                                                   fixture->result, &handle),
              TURBO_SCRIPT_STATUS_OK);
  check(handle != 0);
  return handle;
}

static turbo_script_value_view_t integer_arg(int64_t value) {
  turbo_script_value_view_t arg = {
      .kind = TURBO_SCRIPT_VALUE_INT64,
      .as.integer = value,
  };
  return arg;
}

static void check_error(host_limits_fixture_t *fixture, turbo_script_status_t expected_status,
                        turbo_script_error_phase_t expected_phase, const char *message_part) {
  turbo_script_error_info_t error = {.struct_size = sizeof(error)};
  check_equal(fixture->instance->state, TS_HOST_INSTANCE_FAULTED);
  check_equal(turbo_script_result_get_error(fixture->result, &error), TURBO_SCRIPT_STATUS_OK);
  check_equal(error.status, expected_status);
  check_equal(error.phase, expected_phase);
  check(error.line > 0);
  check(error.column > 0);
  check_not_null(error.message.data);
  if (error.message.data) check_not_null(strstr(error.message.data, message_part));
}

static turbo_script_status_t host_limits_call(host_limits_fixture_t *fixture, const char *name,
                                              const turbo_script_value_view_t *args,
                                              size_t arg_count) {
  turbo_script_export_handle_t handle = host_limits_resolve(fixture, name);
  return turbo_script_instance_call(fixture->instance, handle, args, arg_count, &fixture->options,
                                    fixture->result);
}

static void run_zero_preflight(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t fixture;
  host_limits_fixture_open(&fixture, mode, 0, 0);
  fixture.options.max_steps = 0;
  check_equal(host_limits_call(&fixture, "simple", NULL, 0), TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
  check_equal(fixture.instance->state, TS_HOST_INSTANCE_READY);
  host_limits_fixture_close(&fixture);
}

static void run_step_boundary(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t exact;
  host_limits_fixture_t exceeded;
  turbo_script_value_view_t value = {0};
  host_limits_fixture_open(&exact, mode, 0, 0);
  exact.options.max_steps = 3;
  check_equal(host_limits_call(&exact, "simple", NULL, 0), TURBO_SCRIPT_STATUS_OK);
  check_equal(turbo_script_result_get_value(exact.result, &value), TURBO_SCRIPT_STATUS_OK);
  host_limits_fixture_close(&exact);

  host_limits_fixture_open(&exceeded, mode, 0, 0);
  exceeded.options.max_steps = 2;
  check_equal(host_limits_call(&exceeded, "simple", NULL, 0), TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_error(&exceeded, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL, "step");
  host_limits_fixture_close(&exceeded);
}

static void run_loop_boundary(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t exact;
  host_limits_fixture_t exceeded;
  turbo_script_value_view_t three = integer_arg(3);
  turbo_script_value_view_t four = integer_arg(4);
  host_limits_fixture_open(&exact, mode, 0, 0);
  exact.options.max_loop_iterations = 3;
  check_equal(host_limits_call(&exact, "loop", &three, 1), TURBO_SCRIPT_STATUS_OK);
  host_limits_fixture_close(&exact);

  host_limits_fixture_open(&exceeded, mode, 0, 0);
  exceeded.options.max_loop_iterations = 3;
  check_equal(host_limits_call(&exceeded, "loop", &four, 1), TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_error(&exceeded, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL, "loop");
  host_limits_fixture_close(&exceeded);
}

static void run_hard_limit_clamps(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t loops;
  host_limits_fixture_t recursion;
  host_limits_fixture_t callbacks;
  turbo_script_value_view_t loop_count = integer_arg(TEST_DEFAULT_LOOP_LIMIT + 1);
  turbo_script_value_view_t recursion_depth = integer_arg(TEST_DEFAULT_RECURSION_LIMIT + 1);
  turbo_script_value_view_t callback_count = integer_arg(TEST_DEFAULT_CALLBACK_LIMIT + 1);

  host_limits_fixture_open(&loops, mode, 0, 0);
  loops.options.max_loop_iterations = UINT32_MAX;
  check_equal(host_limits_call(&loops, "loop", &loop_count, 1), TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_error(&loops, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL, "loop");
  host_limits_fixture_close(&loops);

  host_limits_fixture_open_with_recursion(&recursion, mode, 0, 0, UINT32_MAX);
  recursion.options.max_recursion = UINT32_MAX;
  check_equal(host_limits_call(&recursion, "recurse", &recursion_depth, 1),
              TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_error(&recursion, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL,
              "recursion");
  host_limits_fixture_close(&recursion);

  host_limits_fixture_open(&callbacks, mode, 0, 0);
  callbacks.options.max_host_callbacks = UINT32_MAX;
  check_equal(host_limits_call(&callbacks, "callbacks", &callback_count, 1),
              TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_equal(callbacks.host_calls, (size_t)TEST_DEFAULT_CALLBACK_LIMIT);
  check_error(&callbacks, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL,
              "callback");
  host_limits_fixture_close(&callbacks);
}

static void run_callback_boundary(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t exact;
  host_limits_fixture_t exceeded;
  turbo_script_value_view_t one = integer_arg(1);
  turbo_script_value_view_t two = integer_arg(2);
  host_limits_fixture_open(&exact, mode, 0, 0);
  exact.options.max_host_callbacks = 1;
  check_equal(host_limits_call(&exact, "callbacks", &one, 1), TURBO_SCRIPT_STATUS_OK);
  check_equal(exact.host_calls, (size_t)1);
  host_limits_fixture_close(&exact);

  host_limits_fixture_open(&exceeded, mode, 0, 0);
  exceeded.options.max_host_callbacks = 1;
  check_equal(host_limits_call(&exceeded, "callbacks", &two, 1),
              TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_equal(exceeded.host_calls, (size_t)1);
  check_error(&exceeded, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL,
              "callback");
  host_limits_fixture_close(&exceeded);
}

static void run_stack_limit(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t fixture;
  host_limits_fixture_open(&fixture, mode, 1, 0);
  check_equal(host_limits_call(&fixture, "simple", NULL, 0), TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_error(&fixture, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL, "stack");
  host_limits_fixture_close(&fixture);
}

static void run_result_and_retained_limits(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t result_fixture;
  host_limits_fixture_t retained_fixture;
  turbo_script_value_view_t grow_count = integer_arg(2000);
  host_limits_fixture_open(&result_fixture, mode, 0, 0);
  result_fixture.options.max_result_bytes = 1;
  check_equal(host_limits_call(&result_fixture, "large", NULL, 0),
              TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_error(&result_fixture, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL,
              "result");
  host_limits_fixture_close(&result_fixture);

  host_limits_fixture_open(&retained_fixture, mode, 0, TEST_RETAINED_LIMIT);
  retained_fixture.options.max_result_bytes = SIZE_MAX;
  check_equal(host_limits_call(&retained_fixture, "grow", &grow_count, 1),
              TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
  check_error(&retained_fixture, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_CALL,
              "retained");
  host_limits_fixture_close(&retained_fixture);
}

static void run_loop_interrupt(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t fixture;
  interrupt_probe_t probe;
  turbo_script_value_view_t count = integer_arg(100000);
  memset(&probe, 0, sizeof(probe));
  atomic_init(&probe.requested, 0);
  probe.request_after = 2;
  host_limits_fixture_open(&fixture, mode, 0, 0);
  fixture.options.max_loop_iterations = UINT32_MAX;
  fixture.options.max_steps = UINT32_MAX;
  fixture.options.interrupt = test_interrupt;
  fixture.options.interrupt_user_data = &probe;
  check_equal(host_limits_call(&fixture, "loop", &count, 1), TURBO_SCRIPT_STATUS_INTERRUPTED);
  check_greater_equal(probe.checks, (size_t)2);
  check_error(&fixture, TURBO_SCRIPT_STATUS_INTERRUPTED, TURBO_SCRIPT_ERROR_PHASE_INTERRUPT,
              "interrupt");
  host_limits_fixture_close(&fixture);
}

static void run_callback_interrupts(turbo_script_execution_mode_t mode) {
  host_limits_fixture_t before;
  host_limits_fixture_t after;
  interrupt_probe_t before_probe;
  interrupt_probe_t after_probe;

  memset(&before_probe, 0, sizeof(before_probe));
  atomic_init(&before_probe.requested, 0);
  before_probe.request_after = 2;
  host_limits_fixture_open(&before, mode, 0, 0);
  before.options.interrupt = test_interrupt;
  before.options.interrupt_user_data = &before_probe;
  check_equal(host_limits_call(&before, "callback_once", NULL, 0), TURBO_SCRIPT_STATUS_INTERRUPTED);
  check_equal(before.host_calls, (size_t)0);
  check_error(&before, TURBO_SCRIPT_STATUS_INTERRUPTED, TURBO_SCRIPT_ERROR_PHASE_INTERRUPT,
              "interrupt");
  host_limits_fixture_close(&before);

  memset(&after_probe, 0, sizeof(after_probe));
  atomic_init(&after_probe.requested, 0);
  after_probe.request_after = 3;
  host_limits_fixture_open(&after, mode, 0, 0);
  after.options.interrupt = test_interrupt;
  after.options.interrupt_user_data = &after_probe;
  check_equal(host_limits_call(&after, "callback_once", NULL, 0), TURBO_SCRIPT_STATUS_INTERRUPTED);
  check_equal(after.host_calls, (size_t)1);
  check_error(&after, TURBO_SCRIPT_STATUS_INTERRUPTED, TURBO_SCRIPT_ERROR_PHASE_INTERRUPT,
              "interrupt");
  host_limits_fixture_close(&after);
}

spec("TurboScript Host shared limits and safe points") {
  it("rejects zero call limits before interpreter execution") {
    run_zero_preflight(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("rejects zero call limits before JIT execution") { run_zero_preflight(TURBO_SCRIPT_EXEC_JIT); }
  it("charges exact interpreter and JIT step budgets") {
    run_step_boundary(TURBO_SCRIPT_EXEC_INTERPRETER);
    run_step_boundary(TURBO_SCRIPT_EXEC_JIT);
  }
  it("charges exact interpreter and JIT loop budgets") {
    run_loop_boundary(TURBO_SCRIPT_EXEC_INTERPRETER);
    run_loop_boundary(TURBO_SCRIPT_EXEC_JIT);
  }
  it("clamps attempts to widen interpreter hard limits") {
    run_hard_limit_clamps(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("clamps attempts to widen JIT hard limits") { run_hard_limit_clamps(TURBO_SCRIPT_EXEC_JIT); }
  it("charges interpreter and JIT callback budgets") {
    run_callback_boundary(TURBO_SCRIPT_EXEC_INTERPRETER);
    run_callback_boundary(TURBO_SCRIPT_EXEC_JIT);
  }
  it("charges interpreter and JIT frame bytes") {
    run_stack_limit(TURBO_SCRIPT_EXEC_INTERPRETER);
    run_stack_limit(TURBO_SCRIPT_EXEC_JIT);
  }
  it("bounds interpreter and JIT result and retained bytes") {
    run_result_and_retained_limits(TURBO_SCRIPT_EXEC_INTERPRETER);
    run_result_and_retained_limits(TURBO_SCRIPT_EXEC_JIT);
  }
  it("interrupts interpreter and JIT loops after entry") {
    run_loop_interrupt(TURBO_SCRIPT_EXEC_INTERPRETER);
    run_loop_interrupt(TURBO_SCRIPT_EXEC_JIT);
  }
  it("polls interrupt before and after interpreter callbacks") {
    run_callback_interrupts(TURBO_SCRIPT_EXEC_INTERPRETER);
  }
  it("polls interrupt before and after JIT callbacks") {
    run_callback_interrupts(TURBO_SCRIPT_EXEC_JIT);
  }
}
