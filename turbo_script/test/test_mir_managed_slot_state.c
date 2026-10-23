#include "tinytest.h"

#include "../src/mir/turbo_script_mir_internal.h"

#include <stdbool.h>
#include <string.h>

static void state_fixture(
    ts_mir_compiler_t *compiler,
    turbo_script_ctx_t *runtime,
    ts_mir_managed_slot_entry_t *entry) {
  memset(compiler, 0, sizeof(*compiler));
  memset(runtime, 0, sizeof(*runtime));
  memset(entry, 0, sizeof(*entry));
  entry->ptr_reg = (MIR_reg_t)17;
  entry->data = exprtk_value_cmeta_data();
  entry->size = entry->data->storage_type->size;
  entry->state = TS_MIR_MANAGED_SLOT_ZERO;
  compiler->managed_slots = entry;
  compiler->managed_slot_count = 1;
  compiler->managed_slot_capacity = 1;
  compiler->ts_ctx = runtime;
}

static void reset_failure(ts_mir_compiler_t *compiler, turbo_script_ctx_t *runtime) {
  compiler->failed = 0;
  runtime->error_code = TURBO_SCRIPT_ERROR_NONE;
  runtime->error_msg[0] = '\0';
}

spec("TurboScript MIR managed slot ownership state") {
  it("tracks ZERO to LIVE to CLEANED") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check_equal(entry.state, TS_MIR_MANAGED_SLOT_LIVE);
    check(ts_mir_managed_slot_require_readable(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check_equal(entry.state, TS_MIR_MANAGED_SLOT_CLEANED);
    check_false(compiler.failed);
  }

  it("moves through canonical CMeta and rejects source cleanup") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    const cmeta_data_desc *data = exprtk_value_cmeta_data();
    exprtk_value_t source = exprtk_val_num(42.0);
    exprtk_value_t destination;
    bool source_zero = false;

    state_fixture(&compiler, &runtime, &entry);
    check_equal(cmeta_data_value_init_zero(data, &destination), CMETA_OK);
    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_require_readable(&compiler, entry.ptr_reg));

    check_equal(cmeta_data_value_move(data, &destination, &source), CMETA_OK);
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check_equal(entry.state, TS_MIR_MANAGED_SLOT_MOVED);
    check_equal(cmeta_data_value_is_zero(data, &source, &source_zero), CMETA_OK);
    check_true(source_zero);
    check_equal(destination.type, EXPRTK_VAL_NUMBER);
    check_equal(destination.data.number, 42.0);

    check_false(ts_mir_managed_slot_require_readable(&compiler, entry.ptr_reg));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "used after move"));
    reset_failure(&compiler, &runtime);
    check_false(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "cleanup after move"));

    cmeta_data_value_destroy(data, &destination);
  }

  it("rejects double publication") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check_false(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "published while not zero"));
  }

  it("rejects double cleanup") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    reset_failure(&compiler, &runtime);
    check_false(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "cleaned twice"));
  }

  it("rejects cleanup after move") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check_false(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "cleanup after move"));
  }
}
