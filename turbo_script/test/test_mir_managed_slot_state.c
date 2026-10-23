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

    state_fixture(&compiler, &runtime, &entry);
    check_equal(cmeta_data_value_init_zero(data, &destination), CMETA_OK);
    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_require_readable(&compiler, entry.ptr_reg));

    check_equal(cmeta_data_value_move(data, &destination, &source), CMETA_OK);
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check_equal(entry.state, TS_MIR_MANAGED_SLOT_MOVED);
    /* exprtk_value_cmeta_data intentionally does not advertise a generic
     * is_zero query; semantic zero after move is the canonical NULL value. */
    check_equal(source.type, EXPRTK_VAL_NULL);
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

  it("accepts equal ownership states at a branch join") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t baseline = {0};
    ts_mir_managed_slot_snapshot_t then_state = {0};
    ts_mir_managed_slot_snapshot_t else_state = {0};
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &baseline));
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, baseline.count, &then_state));

    check(ts_mir_managed_slot_snapshot_restore(&compiler, &baseline));
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, baseline.count, &else_state));
    check(ts_mir_managed_slot_snapshot_join(&compiler, &then_state, &else_state));
    check_false(compiler.failed);

    ts_mir_managed_slot_snapshot_destroy(&baseline);
    ts_mir_managed_slot_snapshot_destroy(&then_state);
    ts_mir_managed_slot_snapshot_destroy(&else_state);
  }

  it("rejects incompatible ownership states at a branch join") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t baseline = {0};
    ts_mir_managed_slot_snapshot_t then_state = {0};
    ts_mir_managed_slot_snapshot_t else_state = {0};
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &baseline));
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, baseline.count, &then_state));

    check(ts_mir_managed_slot_snapshot_restore(&compiler, &baseline));
    check(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, baseline.count, &else_state));
    check_false(ts_mir_managed_slot_snapshot_join(&compiler, &then_state, &else_state));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "state mismatch at branch join"));

    ts_mir_managed_slot_snapshot_destroy(&baseline);
    ts_mir_managed_slot_snapshot_destroy(&then_state);
    ts_mir_managed_slot_snapshot_destroy(&else_state);
  }

  it("accepts an unchanged managed slot state at a loop backedge") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t entry_state = {0};
    ts_mir_managed_slot_snapshot_t backedge_state = {0};
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &entry_state));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &backedge_state));
    check(ts_mir_managed_slot_snapshot_loop_join(&compiler, &entry_state, &backedge_state));
    check_false(compiler.failed);

    ts_mir_managed_slot_snapshot_destroy(&entry_state);
    ts_mir_managed_slot_snapshot_destroy(&backedge_state);
  }

  it("rejects a changed managed slot state at a loop backedge") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t entry_state = {0};
    ts_mir_managed_slot_snapshot_t backedge_state = {0};
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &entry_state));
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &backedge_state));
    check_false(ts_mir_managed_slot_snapshot_loop_join(
        &compiler, &entry_state, &backedge_state));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "state mismatch at loop backedge"));

    ts_mir_managed_slot_snapshot_destroy(&entry_state);
    ts_mir_managed_slot_snapshot_destroy(&backedge_state);
  }

  it("accepts break and continue edges that preserve loop-entry state") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t entry_state = {0};
    loop_frame_t *frame;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &entry_state));
    frame = &compiler.loop_stack[0];
    frame->first_managed_slot = 1u;
    frame->entry_state = &entry_state;
    compiler.loop_depth = 1;

    check(ts_mir_managed_slot_record_loop_edge(&compiler, 0));
    check(ts_mir_managed_slot_record_loop_edge(&compiler, 1));
    check(ts_mir_managed_slot_validate_loop_edges(
        &compiler, frame, &entry_state, &entry_state));
    check_false(compiler.failed);

    ts_mir_loop_edge_states_destroy(frame);
    ts_mir_managed_slot_snapshot_destroy(&entry_state);
  }

  it("rejects a loop control edge with a live branch-local slot") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t entry_state = {0};
    loop_frame_t *frame;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_snapshot_capture(&compiler, 0u, &entry_state));
    frame = &compiler.loop_stack[0];
    frame->first_managed_slot = 0u;
    frame->entry_state = &entry_state;
    compiler.loop_depth = 1;
    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));

    check_false(ts_mir_managed_slot_record_loop_edge(&compiler, 0));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "leaves branch with live cleanup obligation"));

    ts_mir_loop_edge_states_destroy(frame);
    ts_mir_managed_slot_snapshot_destroy(&entry_state);
  }

  it("rejects a break edge that changes a pre-existing slot state") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t entry_state = {0};
    loop_frame_t *frame;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &entry_state));
    frame = &compiler.loop_stack[0];
    frame->first_managed_slot = 1u;
    frame->entry_state = &entry_state;
    compiler.loop_depth = 1;

    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_record_loop_edge(&compiler, 0));
    check_false(ts_mir_managed_slot_validate_loop_edges(
        &compiler, frame, &entry_state, &entry_state));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "state mismatch at loop exit"));

    ts_mir_loop_edge_states_destroy(frame);
    ts_mir_managed_slot_snapshot_destroy(&entry_state);
  }

  it("rejects a continue edge that disagrees with the continue target state") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    ts_mir_managed_slot_snapshot_t entry_state = {0};
    ts_mir_managed_slot_snapshot_t continue_state = {0};
    loop_frame_t *frame;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &entry_state));
    frame = &compiler.loop_stack[0];
    frame->first_managed_slot = 1u;
    frame->entry_state = &entry_state;
    compiler.loop_depth = 1;

    check(ts_mir_managed_slot_record_loop_edge(&compiler, 1));
    check(ts_mir_managed_slot_move(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_snapshot_capture(&compiler, 1u, &continue_state));
    check_false(ts_mir_managed_slot_validate_loop_edges(
        &compiler, frame, &entry_state, &continue_state));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "state mismatch at continue target"));

    ts_mir_loop_edge_states_destroy(frame);
    ts_mir_managed_slot_snapshot_destroy(&entry_state);
    ts_mir_managed_slot_snapshot_destroy(&continue_state);
  }

  it("accepts a return when all managed slots are terminal") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slots_require_terminal_at_return(&compiler));
    check_false(compiler.failed);
  }

  it("rejects a return with a live managed slot obligation") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check_false(ts_mir_managed_slots_require_terminal_at_return(&compiler));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "return with live cleanup obligation"));

    reset_failure(&compiler, &runtime);
    check(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slots_require_terminal_at_return(&compiler));
    check_false(compiler.failed);
  }

  it("accepts a throw edge when managed slots are terminal") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slots_require_terminal_at_throw(&compiler));
    check_false(compiler.failed);
  }

  it("rejects a throw edge with a live managed slot obligation") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check_false(ts_mir_managed_slots_require_terminal_at_throw(&compiler));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "throw with live cleanup obligation"));

    reset_failure(&compiler, &runtime);
    check(ts_mir_managed_slot_cleanup(&compiler, entry.ptr_reg));
    check(ts_mir_managed_slots_require_terminal_at_throw(&compiler));
    check_false(compiler.failed);
  }

  it("rejects a branch-local slot that leaves a live cleanup obligation") {
    ts_mir_compiler_t compiler;
    turbo_script_ctx_t runtime;
    ts_mir_managed_slot_entry_t entry;
    state_fixture(&compiler, &runtime, &entry);

    check(ts_mir_managed_slots_require_terminal_since(&compiler, 0u));
    check(ts_mir_managed_slot_publish(&compiler, entry.ptr_reg));
    check_false(ts_mir_managed_slots_require_terminal_since(&compiler, 0u));
    check(compiler.failed);
    check_not_null(strstr(runtime.error_msg, "leaves branch with live cleanup obligation"));
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
