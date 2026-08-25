#include "tinytest.h"

#include "exprtk.h"
#include "turbo/thread.h"
#include "turbo_script_host_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

turbo_script_status_t ts_host_check_owner_thread(const turbo_script_ctx_t *ctx);
turbo_script_status_t ts_host_registry_acquire_module(turbo_script_ctx_t *ctx);
turbo_script_status_t ts_host_registry_release_module(turbo_script_ctx_t *ctx);
turbo_script_status_t ts_host_registry_find_slot(const turbo_script_ctx_t *ctx,
                                                 turbo_script_string_view_t name,
                                                 size_t *out_slot);
turbo_script_status_t ts_host_registry_bind_runtime(turbo_script_ctx_t *ctx,
                                                    exprtk_env_t *runtime_ctx);

enum { TEST_HOST_REGISTRY_CAPACITY = 256 };

typedef struct registry_thread_probe_s {
  turbo_script_ctx_t *ctx;
  turbo_script_result_t *result;
  turbo_script_host_function_descriptor_t descriptor;
  turbo_script_status_t owner_status;
  turbo_script_status_t register_status;
  turbo_script_status_t unregister_status;
  turbo_script_status_t bind_status;
} registry_thread_probe_t;

typedef struct callback_probe_s {
  size_t calls;
  turbo_script_status_t second_write_status;
} callback_probe_t;

static turbo_script_host_function_descriptor_t descriptor_for(const char *name) {
  turbo_script_host_function_descriptor_t descriptor = {
      .struct_size = sizeof(descriptor),
      .min_arity = 1,
      .max_arity = 1,
      .name = {name, strlen(name)},
  };
  return descriptor;
}

static turbo_script_status_t host_echo(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder) {
  (void)user_data;
  if (arg_count != 1) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  return turbo_script_host_result_set_value(builder, &args[0]);
}

static turbo_script_status_t host_empty_success(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  (void)args;
  (void)arg_count;
  (void)builder;
  probe->calls++;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t host_write_twice(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  turbo_script_value_view_t value = {
      .kind = TURBO_SCRIPT_VALUE_INT64,
      .as.integer = 23,
  };
  turbo_script_string_view_t message = {"ignored", 7};
  (void)args;
  (void)arg_count;
  probe->calls++;
  if (turbo_script_host_result_set_value(builder, &value) != TURBO_SCRIPT_STATUS_OK)
    return TURBO_SCRIPT_STATUS_HOST_ERROR;
  probe->second_write_status = turbo_script_host_result_set_error(builder, -9, message);
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t host_error(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  turbo_script_string_view_t message = {"host callback rejected input", 28};
  (void)args;
  (void)arg_count;
  probe->calls++;
  return turbo_script_host_result_set_error(builder, -77, message);
}

static void registry_thread_probe_run(void *arg) {
  registry_thread_probe_t *probe = (registry_thread_probe_t *)arg;
  exprtk_env_t runtime;
  exprtk_env_init(&runtime);
  probe->owner_status = ts_host_check_owner_thread(probe->ctx);
  probe->register_status = turbo_script_context_register_host_function(
      probe->ctx, &probe->descriptor, host_echo, NULL, probe->result);
  probe->unregister_status = turbo_script_context_unregister_host_function(
      probe->ctx, probe->descriptor.name, probe->result);
  probe->bind_status = ts_host_registry_bind_runtime(probe->ctx, &runtime);
  exprtk_env_free(&runtime);
}

static void bind_one(turbo_script_ctx_t *ctx, turbo_script_result_t *result,
                     const char *name, turbo_script_host_function_t callback,
                     void *user_data, exprtk_env_t *runtime) {
  turbo_script_host_function_descriptor_t descriptor = descriptor_for(name);
  check_equal(turbo_script_context_register_host_function(
                  ctx, &descriptor, callback, user_data, result),
              TURBO_SCRIPT_STATUS_OK);
  check_equal(ts_host_registry_acquire_module(ctx), TURBO_SCRIPT_STATUS_OK);
  exprtk_env_init(runtime);
  check_equal(ts_host_registry_bind_runtime(ctx, runtime), TURBO_SCRIPT_STATUS_OK);
}

static void unbind_one(turbo_script_ctx_t *ctx, exprtk_env_t *runtime) {
  exprtk_env_free(runtime);
  check_equal(ts_host_registry_release_module(ctx), TURBO_SCRIPT_STATUS_OK);
}

spec("turbo_script_host_registry") {
  static turbo_script_ctx_t *ctx;
  static turbo_script_result_t *result;

  before_each() {
    ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    result = NULL;
    check_not_null(ctx);
    check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
    check_not_null(result);
  }

  after_each() {
    turbo_script_result_destroy(result);
    turbo_script_free(ctx);
    result = NULL;
    ctx = NULL;
  }

  describe("descriptor validation") {
    it("rejects a descriptor whose struct_size is too small") {
      turbo_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      descriptor.struct_size = sizeof(descriptor) - 1;

      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
    }

    it("rejects empty invalid UTF-8 and embedded-NUL names") {
      const char invalid[] = "\xC0\xAF";
      const char embedded_nul[] = {'e', '\0', 'x'};
      turbo_script_host_function_descriptor_t descriptor = descriptor_for("echo");

      descriptor.name.data = NULL;
      descriptor.name.size = 0;
      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_VALIDATION_ERROR);

      descriptor.name.data = invalid;
      descriptor.name.size = sizeof(invalid) - 1;
      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_INVALID_UTF8);

      descriptor.name.data = embedded_nul;
      descriptor.name.size = sizeof(embedded_nul);
      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_VALIDATION_ERROR);
    }

    it("rejects reversed arity bounds") {
      turbo_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      descriptor.min_arity = 2;
      descriptor.max_arity = 1;

      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
    }

    it("rejects nonzero reserved descriptor fields") {
      turbo_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      descriptor.reserved[3] = 1;

      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
    }
  }

  describe("mutable registry") {
    it("owns the registered name bytes") {
      char name[] = "owned_echo";
      turbo_script_host_function_descriptor_t descriptor = descriptor_for(name);
      exprtk_env_t runtime;
      exprtk_value_t argument = exprtk_val_bool(1);
      exprtk_value_t actual;

      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_OK);
      memset(name, 'x', sizeof(name) - 1);
      check_equal(ts_host_registry_acquire_module(ctx), TURBO_SCRIPT_STATUS_OK);
      exprtk_env_init(&runtime);
      check_equal(ts_host_registry_bind_runtime(ctx, &runtime), TURBO_SCRIPT_STATUS_OK);
      actual = exprtk_call_internal("owned_echo", 1, &argument, &runtime);
      check_equal(actual.type, EXPRTK_VAL_BOOL);
      check_equal(actual.data.boolean, 1);
      exprtk_value_destroy(&actual);
      exprtk_env_free(&runtime);
      check_equal(ts_host_registry_release_module(ctx), TURBO_SCRIPT_STATUS_OK);
    }

    it("rejects a duplicate name and reports missing unregister") {
      turbo_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      turbo_script_string_view_t missing = {"missing", 7};

      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(turbo_script_context_unregister_host_function(ctx, missing, result),
                  TURBO_SCRIPT_STATUS_NOT_FOUND);
    }

    it("publishes a structured error and clears it on the next success") {
      turbo_script_host_function_descriptor_t invalid = descriptor_for("bad");
      turbo_script_host_function_descriptor_t valid = descriptor_for("good");
      turbo_script_error_info_t error = {0};
      invalid.min_arity = 2;
      invalid.max_arity = 1;

      check_equal(turbo_script_context_register_host_function(
                      ctx, &invalid, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(turbo_script_result_get_error(result, &error), TURBO_SCRIPT_STATUS_OK);
      check_equal(error.status, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(error.phase, TURBO_SCRIPT_ERROR_PHASE_NONE);
      check_greater(error.message.size, (size_t)0);

      check_equal(turbo_script_context_register_host_function(
                      ctx, &valid, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_result_get_error(result, &error),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
    }

    it("caps the registry at 256 slots") {
      char name[32];
      for (size_t i = 0; i < TEST_HOST_REGISTRY_CAPACITY; ++i) {
        turbo_script_host_function_descriptor_t descriptor;
        check_greater(snprintf(name, sizeof(name), "host_%03zu", i), 0);
        descriptor = descriptor_for(name);
        check_equal(turbo_script_context_register_host_function(
                        ctx, &descriptor, host_echo, NULL, result),
                    TURBO_SCRIPT_STATUS_OK);
      }
      {
        turbo_script_host_function_descriptor_t descriptor = descriptor_for("overflow");
        check_equal(turbo_script_context_register_host_function(
                        ctx, &descriptor, host_echo, NULL, result),
                    TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
      }
    }

    it("rejects registry operations from a non-owner TurboUtils thread") {
      turbo_thread_t thread = NULL;
      registry_thread_probe_t probe = {
          .ctx = ctx,
          .result = result,
          .descriptor = descriptor_for("echo"),
      };

      check_equal(turbo_thread_create(&thread, registry_thread_probe_run, &probe), 0);
      check_equal(turbo_thread_join(&thread), 0);
      turbo_thread_destroy(&thread);
      check_equal(probe.owner_status, TURBO_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.register_status, TURBO_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.unregister_status, TURBO_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.bind_status, TURBO_SCRIPT_STATUS_WRONG_THREAD);
    }
  }

  describe("freeze boundary") {
    it("rejects mutation while any host module is active") {
      turbo_script_host_function_descriptor_t first = descriptor_for("first");
      turbo_script_host_function_descriptor_t second = descriptor_for("second");

      check_equal(turbo_script_context_register_host_function(
                      ctx, &first, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_acquire_module(ctx), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_context_register_host_function(
                      ctx, &second, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(turbo_script_context_unregister_host_function(ctx, first.name, result),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(ts_host_registry_release_module(ctx), TURBO_SCRIPT_STATUS_OK);
    }

    it("unfreezes only after the last module releases the registry") {
      turbo_script_host_function_descriptor_t descriptor = descriptor_for("echo");

      check_equal(turbo_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_acquire_module(ctx), TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_acquire_module(ctx), TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_release_module(ctx), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_context_unregister_host_function(ctx, descriptor.name, result),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(ts_host_registry_release_module(ctx), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_context_unregister_host_function(ctx, descriptor.name, result),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_release_module(ctx), TURBO_SCRIPT_STATUS_INVALID_STATE);
    }

    it("keeps resolved slot indexes stable for the frozen lifetime") {
      turbo_script_host_function_descriptor_t first = descriptor_for("first");
      turbo_script_host_function_descriptor_t second = descriptor_for("second");
      size_t before_freeze = SIZE_MAX;
      size_t while_frozen = SIZE_MAX;

      check_equal(turbo_script_context_register_host_function(
                      ctx, &first, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_context_register_host_function(
                      ctx, &second, host_echo, NULL, result),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_find_slot(ctx, second.name, &before_freeze),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(before_freeze, (size_t)1);
      check_equal(ts_host_registry_acquire_module(ctx), TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_find_slot(ctx, second.name, &while_frozen),
                  TURBO_SCRIPT_STATUS_OK);
      check_equal(while_frozen, before_freeze);
      check_equal(ts_host_registry_release_module(ctx), TURBO_SCRIPT_STATUS_OK);
    }
  }

  describe("callback adapter and terminal builder") {
    it("passes borrowed arguments and returns one host value") {
      exprtk_env_t runtime;
      exprtk_value_t argument = exprtk_val_bool(1);
      exprtk_value_t actual;
      bind_one(ctx, result, "echo", host_echo, NULL, &runtime);

      actual = exprtk_call_internal("echo", 1, &argument, &runtime);
      check_equal(actual.type, EXPRTK_VAL_BOOL);
      check_equal(actual.data.boolean, 1);
      check_equal(runtime.flow, exprtk_FLOW_NORMAL);
      exprtk_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("turns callback success with an empty builder into a host error") {
      callback_probe_t probe = {0};
      exprtk_env_t runtime;
      exprtk_value_t argument = exprtk_val_bool(1);
      exprtk_value_t actual;
      bind_one(ctx, result, "empty", host_empty_success, &probe, &runtime);

      actual = exprtk_call_internal("empty", 1, &argument, &runtime);
      check_equal(probe.calls, (size_t)1);
      check_equal(runtime.flow, exprtk_FLOW_THROW);
      check_not_null(strstr(runtime.error_msg, "HOST_ERROR"));
      exprtk_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("rejects a second builder write and keeps the first value") {
      callback_probe_t probe = {0};
      exprtk_env_t runtime;
      exprtk_value_t argument = exprtk_val_bool(1);
      exprtk_value_t actual;
      bind_one(ctx, result, "twice", host_write_twice, &probe, &runtime);

      actual = exprtk_call_internal("twice", 1, &argument, &runtime);
      check_equal(probe.calls, (size_t)1);
      check_equal(probe.second_write_status, TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(actual.type, EXPRTK_VAL_INTEGER);
      check_equal(actual.data.integer, INT64_C(23));
      check_equal(runtime.flow, exprtk_FLOW_NORMAL);
      exprtk_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("maps a builder error into exprtk throw state") {
      callback_probe_t probe = {0};
      exprtk_env_t runtime;
      exprtk_value_t argument = exprtk_val_bool(1);
      exprtk_value_t actual;
      bind_one(ctx, result, "fail", host_error, &probe, &runtime);

      actual = exprtk_call_internal("fail", 1, &argument, &runtime);
      check_equal(probe.calls, (size_t)1);
      check_equal(runtime.flow, exprtk_FLOW_THROW);
      check_not_null(strstr(runtime.error_msg, "host callback rejected input"));
      exprtk_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("enforces the registered arity before invoking the callback") {
      callback_probe_t probe = {0};
      exprtk_env_t runtime;
      exprtk_value_t actual;
      bind_one(ctx, result, "arity", host_empty_success, &probe, &runtime);

      actual = exprtk_call_internal("arity", 0, NULL, &runtime);
      check_equal(probe.calls, (size_t)0);
      check_equal(runtime.flow, exprtk_FLOW_THROW);
      check_not_null(strstr(runtime.error_msg, "arity"));
      exprtk_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }
  }
}
