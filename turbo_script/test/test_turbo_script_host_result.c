#include "tinytest.h"

#include "exprtk_module.h"
#include "turbo/thread.h"
#include "turbo_script_host_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  TEST_MAX_DEPTH = 64,
  TEST_MAX_NODES = 65536,
  TEST_MAX_BYTES = 4 * 1024 * 1024,
};

static ts_host_value_limits_t test_limits(void) {
  ts_host_value_limits_t limits = {
      TEST_MAX_DEPTH,
      TEST_MAX_NODES,
      TEST_MAX_BYTES,
  };
  return limits;
}

static void check_no_published_value(turbo_script_result_t *result) {
  turbo_script_value_view_t actual = {0};
  check_equal(turbo_script_result_get_value(result, &actual), TURBO_SCRIPT_STATUS_INVALID_STATE);
}

typedef struct result_thread_probe_s {
  turbo_script_result_t *result;
  turbo_script_ctx_t *ctx;
  turbo_script_status_t getter_status;
  turbo_script_status_t context_status;
} result_thread_probe_t;

static void result_thread_probe_run(void *arg) {
  result_thread_probe_t *probe = (result_thread_probe_t *)arg;
  turbo_script_value_view_t value = {0};
  probe->getter_status = turbo_script_result_get_value(probe->result, &value);
  probe->context_status = ts_host_result_check_context(probe->result, probe->ctx);
}

spec("turbo_script_host_result") {
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

  describe("owned result values") {
    it("round-trips every scalar kind") {
      const turbo_script_value_view_t inputs[] = {
          {.kind = TURBO_SCRIPT_VALUE_NULL},
          {.kind = TURBO_SCRIPT_VALUE_BOOL, .as.boolean = 1},
          {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = INT64_C(-42)},
          {.kind = TURBO_SCRIPT_VALUE_NUMBER, .as.number = 3.25},
          {.kind = TURBO_SCRIPT_VALUE_STRING, .as.string = {"hello", 5}},
      };
      ts_host_value_limits_t limits = test_limits();

      for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        turbo_script_value_view_t actual = {0};
        check_equal(ts_host_result_store_view(result, &inputs[i], &limits), TURBO_SCRIPT_STATUS_OK);
        check_equal(turbo_script_result_get_value(result, &actual), TURBO_SCRIPT_STATUS_OK);
        check_equal(actual.kind, inputs[i].kind);
        if (inputs[i].kind == TURBO_SCRIPT_VALUE_BOOL) check_equal(actual.as.boolean, (uint8_t)1);
        if (inputs[i].kind == TURBO_SCRIPT_VALUE_INT64)
          check_equal(actual.as.integer, INT64_C(-42));
        if (inputs[i].kind == TURBO_SCRIPT_VALUE_NUMBER) check_equal(actual.as.number, 3.25);
        if (inputs[i].kind == TURBO_SCRIPT_VALUE_STRING) {
          check_equal(actual.as.string.size, (size_t)5);
          check_equal(actual.as.string.data, "hello");
        }
      }
    }

    it("preserves an empty string") {
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_STRING,
          .as.string = {NULL, 0},
      };
      turbo_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_result_get_value(result, &actual), TURBO_SCRIPT_STATUS_OK);
      check_equal(actual.kind, TURBO_SCRIPT_VALUE_STRING);
      check_equal(actual.as.string.size, (size_t)0);
      check_not_null(actual.as.string.data);
      check_equal(actual.as.string.data, "");
    }

    it("preserves an empty array") {
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_ARRAY,
          .as.array = {NULL, 0},
      };
      turbo_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_result_get_value(result, &actual), TURBO_SCRIPT_STATUS_OK);
      check_equal(actual.kind, TURBO_SCRIPT_VALUE_ARRAY);
      check_equal(actual.as.array.count, (size_t)0);
      check_null(actual.as.array.items);
    }

    it("preserves an empty record") {
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_RECORD,
          .as.record = {NULL, 0},
      };
      turbo_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_result_get_value(result, &actual), TURBO_SCRIPT_STATUS_OK);
      check_equal(actual.kind, TURBO_SCRIPT_VALUE_RECORD);
      check_equal(actual.as.record.count, (size_t)0);
      check_null(actual.as.record.entries);
    }

    it("deep-copies nested arrays records keys and strings") {
      char key[] = "items";
      char text[] = "alpha";
      turbo_script_value_view_t items[2] = {
          {.kind = TURBO_SCRIPT_VALUE_STRING, .as.string = {text, 5}},
          {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = 7},
      };
      turbo_script_record_entry_view_t entries[1] = {
          {{key, 5}, {.kind = TURBO_SCRIPT_VALUE_ARRAY, .as.array = {items, 2}}},
      };
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_RECORD,
          .as.record = {entries, 1},
      };
      turbo_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TURBO_SCRIPT_STATUS_OK);
      memset(key, 'x', sizeof(key) - 1);
      memset(text, 'y', sizeof(text) - 1);
      items[1].as.integer = 99;

      check_equal(turbo_script_result_get_value(result, &actual), TURBO_SCRIPT_STATUS_OK);
      check_equal(actual.as.record.entries[0].key.data, "items");
      check_equal(actual.as.record.entries[0].value.as.array.count, (size_t)2);
      check_equal(actual.as.record.entries[0].value.as.array.items[0].as.string.data, "alpha");
      check_equal(actual.as.record.entries[0].value.as.array.items[1].as.integer, INT64_C(7));
    }
  }

  describe("borrowed value validation") {
    it("rejects non-empty views with null backing pointers") {
      const turbo_script_value_view_t inputs[] = {
          {.kind = TURBO_SCRIPT_VALUE_STRING, .as.string = {NULL, 1}},
          {.kind = TURBO_SCRIPT_VALUE_ARRAY, .as.array = {NULL, 1}},
          {.kind = TURBO_SCRIPT_VALUE_RECORD, .as.record = {NULL, 1}},
      };
      ts_host_value_limits_t limits = test_limits();

      for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        check_equal(ts_host_result_store_view(result, &inputs[i], &limits),
                    TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
        check_no_published_value(result);
      }
    }

    it("rejects invalid UTF-8 without publishing a value") {
      const char invalid[] = "\xC0\xAF";
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_STRING,
          .as.string = {invalid, sizeof(invalid) - 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TURBO_SCRIPT_STATUS_INVALID_UTF8);
      check_no_published_value(result);
    }

    it("rejects an invalid UTF-8 record key") {
      const char invalid[] = "\xED\xA0\x80";
      turbo_script_record_entry_view_t entry = {{invalid, sizeof(invalid) - 1},
                                                {.kind = TURBO_SCRIPT_VALUE_NULL}};
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_RECORD,
          .as.record = {&entry, 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_validate_value_view(&input, &limits), TURBO_SCRIPT_STATUS_INVALID_UTF8);
    }

    it("rejects an empty record key") {
      turbo_script_record_entry_view_t entry = {{NULL, 0}, {.kind = TURBO_SCRIPT_VALUE_NULL}};
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_RECORD,
          .as.record = {&entry, 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TURBO_SCRIPT_STATUS_VALIDATION_ERROR);
      check_no_published_value(result);
    }

    it("rejects duplicate record keys without publishing a value") {
      turbo_script_record_entry_view_t entries[2] = {
          {{"id", 2}, {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = 1}},
          {{"id", 2}, {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = 2}},
      };
      turbo_script_value_view_t value = {
          .kind = TURBO_SCRIPT_VALUE_RECORD,
          .as.record = {entries, 2},
      };
      turbo_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &value, &limits),
                  TURBO_SCRIPT_STATUS_DUPLICATE_RECORD_KEY);
      check_equal(turbo_script_result_get_value(result, &actual),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
    }

    it("rejects a cyclic input graph") {
      turbo_script_value_view_t input = {.kind = TURBO_SCRIPT_VALUE_ARRAY};
      ts_host_value_limits_t limits = test_limits();
      input.as.array.items = &input;
      input.as.array.count = 1;

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TURBO_SCRIPT_STATUS_VALIDATION_ERROR);
      check_no_published_value(result);
    }

    it("rejects value depth 65") {
      turbo_script_value_view_t nodes[65] = {0};
      ts_host_value_limits_t limits = test_limits();
      for (size_t i = 0; i + 1 < sizeof(nodes) / sizeof(nodes[0]); ++i) {
        nodes[i].kind = TURBO_SCRIPT_VALUE_ARRAY;
        nodes[i].as.array.items = &nodes[i + 1];
        nodes[i].as.array.count = 1;
      }
      nodes[64].kind = TURBO_SCRIPT_VALUE_NULL;

      check_equal(ts_host_result_store_view(result, &nodes[0], &limits),
                  TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
    }

    it("rejects node count 65537") {
      turbo_script_value_view_t *items =
          (turbo_script_value_view_t *)calloc(TEST_MAX_NODES, sizeof(*items));
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_ARRAY,
          .as.array = {items, TEST_MAX_NODES},
      };
      ts_host_value_limits_t limits = test_limits();
      limits.max_bytes = SIZE_MAX;
      check_not_null(items);

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
      free(items);
    }

    it("rejects byte quota plus one") {
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_STRING,
          .as.string = {"12345", 5},
      };
      ts_host_value_limits_t limits = test_limits();
      limits.max_bytes = 4;

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
    }

    it("rejects SIZE_MAX copy arithmetic overflow") {
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_STRING,
          .as.string = {"x", SIZE_MAX},
      };
      ts_host_value_limits_t limits = test_limits();
      limits.max_bytes = SIZE_MAX;

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
    }
  }

  describe("exprtk conversion") {
    it("converts a nested value into the target context allocator") {
      char key[] = "values";
      char text[] = "owned";
      turbo_script_value_view_t array_items[3] = {
          {.kind = TURBO_SCRIPT_VALUE_BOOL, .as.boolean = 1},
          {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = 23},
          {.kind = TURBO_SCRIPT_VALUE_STRING, .as.string = {text, 5}},
      };
      turbo_script_record_entry_view_t entries[1] = {
          {{key, 6}, {.kind = TURBO_SCRIPT_VALUE_ARRAY, .as.array = {array_items, 3}}},
      };
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_RECORD,
          .as.record = {entries, 1},
      };
      exprtk_value_t actual = {0};
      exprtk_value_t values;
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_value_from_view(ctx, &input, &limits, &actual), TURBO_SCRIPT_STATUS_OK);
      memset(key, 'x', sizeof(key) - 1);
      memset(text, 'y', sizeof(text) - 1);
      check_equal(actual.type, EXPRTK_VAL_MAP);
      values = exprtk_map_get(&actual, "values");
      check_equal(values.type, EXPRTK_VAL_LIST);
      check_equal(values.data.list.count, (size_t)3);
      check_equal(values.data.list.items[0].type, EXPRTK_VAL_BOOL);
      check_equal(values.data.list.items[0].data.boolean, 1);
      check_equal(values.data.list.items[1].type, EXPRTK_VAL_INTEGER);
      check_equal(values.data.list.items[1].data.integer, INT64_C(23));
      check_equal(values.data.list.items[2].type, EXPRTK_VAL_STRING);
      check_equal(values.data.list.items[2].data.string.len, (size_t)5);
      check_equal(values.data.list.items[2].data.string.data, "owned");
      exprtk_value_destroy(&actual);
    }

    it("leaves no output ownership when conversion fails") {
      turbo_script_value_view_t input = {
          .kind = TURBO_SCRIPT_VALUE_STRING,
          .as.string = {NULL, 1},
      };
      exprtk_value_t actual = exprtk_val_int(99);
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_value_from_view(ctx, &input, &limits, &actual),
                  TURBO_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(actual.type, EXPRTK_VAL_NULL);
      check_null(actual.storage);
      check_null(actual.storage_aux);
    }
  }

  describe("result lifecycle") {
    it("reuses one result while keeping value and error mutually exclusive") {
      turbo_script_value_view_t value = {
          .kind = TURBO_SCRIPT_VALUE_STRING,
          .as.string = {"first", 5},
      };
      turbo_script_error_info_t error = {
          .struct_size = sizeof(error),
          .status = TURBO_SCRIPT_STATUS_RUNTIME_ERROR,
          .error_code = 17,
          .phase = TURBO_SCRIPT_ERROR_PHASE_CALL,
          .line = 3,
          .column = 4,
          .length = 2,
          .cause_code = -7,
          .module_name = {"module", 6},
          .function_name = {"run", 3},
          .message = {"failed", 6},
      };
      turbo_script_value_view_t actual_value = {0};
      turbo_script_error_info_t actual_error = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &value, &limits), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_result_get_error(result, &actual_error),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);

      check_equal(ts_host_result_set_error(result, &error, TEST_MAX_BYTES), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_result_get_value(result, &actual_value),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(turbo_script_result_get_error(result, &actual_error), TURBO_SCRIPT_STATUS_OK);
      check_equal(actual_error.status, TURBO_SCRIPT_STATUS_RUNTIME_ERROR);
      check_equal(actual_error.module_name.data, "module");
      check_equal(actual_error.function_name.data, "run");
      check_equal(actual_error.message.data, "failed");

      value.as.string.data = "second";
      value.as.string.size = 6;
      check_equal(ts_host_result_store_view(result, &value, &limits), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_script_result_get_error(result, &actual_error),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(turbo_script_result_get_value(result, &actual_value), TURBO_SCRIPT_STATUS_OK);
      check_equal(actual_value.as.string.data, "second");

      turbo_script_result_reset(result);
      check_equal(turbo_script_result_get_value(result, &actual_value),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
      check_equal(turbo_script_result_get_error(result, &actual_error),
                  TURBO_SCRIPT_STATUS_INVALID_STATE);
    }

    it("deep-copies error views") {
      char module[] = "mod";
      char function[] = "fn";
      char message[] = "boom";
      turbo_script_error_info_t input = {
          .struct_size = sizeof(input),
          .status = TURBO_SCRIPT_STATUS_HOST_ERROR,
          .phase = TURBO_SCRIPT_ERROR_PHASE_HOST_CALLBACK,
          .module_name = {module, 3},
          .function_name = {function, 2},
          .message = {message, 4},
      };
      turbo_script_error_info_t actual = {0};

      check_equal(ts_host_result_set_error(result, &input, TEST_MAX_BYTES), TURBO_SCRIPT_STATUS_OK);
      memset(module, 'x', sizeof(module) - 1);
      memset(function, 'y', sizeof(function) - 1);
      memset(message, 'z', sizeof(message) - 1);
      check_equal(turbo_script_result_get_error(result, &actual), TURBO_SCRIPT_STATUS_OK);
      check_equal(actual.module_name.data, "mod");
      check_equal(actual.function_name.data, "fn");
      check_equal(actual.message.data, "boom");
    }

    it("detects wrong-context access through the internal boundary helper") {
      turbo_script_ctx_t *other = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_not_null(other);

      check_equal(ts_host_result_check_context(result, ctx), TURBO_SCRIPT_STATUS_OK);
      check_equal(ts_host_result_check_context(result, other),
                  TURBO_SCRIPT_STATUS_CONTEXT_MISMATCH);
      turbo_script_free(other);
    }

    it("rejects result access from a non-owner thread") {
      turbo_script_value_view_t value = {.kind = TURBO_SCRIPT_VALUE_NULL};
      ts_host_value_limits_t limits = test_limits();
      turbo_thread_t thread = NULL;
      result_thread_probe_t probe = {
          .result = result,
          .ctx = ctx,
      };

      check_equal(ts_host_result_store_view(result, &value, &limits), TURBO_SCRIPT_STATUS_OK);
      check_equal(turbo_thread_create(&thread, result_thread_probe_run, &probe), 0);
      check_equal(turbo_thread_join(&thread), 0);
      turbo_thread_destroy(&thread);
      check_equal(probe.getter_status, TURBO_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.context_status, TURBO_SCRIPT_STATUS_WRONG_THREAD);
    }
  }
}
