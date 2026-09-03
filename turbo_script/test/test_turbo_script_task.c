#include "tinytest.h"
#include "turbo_script.h"
#include <windows.h>
static void wait_for_tasks(turbo_script_ctx_t *ctx) { for (int i=0;i<200&&turbo_script_task_active_count(ctx);++i) Sleep(5); }
suite("TurboScript Executor tasks") {
  it("runs without a host coroutine context") {
    turbo_script_ctx_t *ctx=turbo_script_init(TURBO_SCRIPT_INIT_BARE); check_not_null(ctx); if(!ctx)return;
    check_equal(turbo_script_run(ctx,"id = task.spawn(() => { task.sleep(10); return 7; });"),0);
    wait_for_tasks(ctx); check_equal(turbo_script_task_active_count(ctx),(size_t)0);
    check_equal(turbo_script_run(ctx,"value = task.result(id);"),0); turbo_script_free(ctx);
  }
  it("cancels a sleeping task") {
    turbo_script_ctx_t *ctx=turbo_script_init(TURBO_SCRIPT_INIT_BARE); check_not_null(ctx); if(!ctx)return;
    check_equal(turbo_script_run(ctx,"id = task.spawn(() => { task.sleep(1000); return 1; }); task.cancel(id);"),0);
    wait_for_tasks(ctx); check_equal(turbo_script_task_active_count(ctx),(size_t)0); turbo_script_free(ctx);
  }
}
