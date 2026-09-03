#include "tinytest.h"
#include "turbo_script.h"
#include <windows.h>
static void wait_for_timers(turbo_script_ctx_t *ctx) { for (int i=0;i<200&&turbo_script_timer_active_count(ctx);++i) Sleep(5); }
suite("TurboScript Executor timers") {
  it("fires after without a host coroutine context") {
    turbo_script_ctx_t *ctx=turbo_script_init(TURBO_SCRIPT_INIT_BARE); check_not_null(ctx); if(!ctx)return;
    check_equal(turbo_script_run(ctx,"timer.after(10, () => { fired = 1; });"),0);
    wait_for_timers(ctx); check_equal(turbo_script_timer_active_count(ctx),(size_t)0); turbo_script_free(ctx);
  }
  it("cancels an interval") {
    turbo_script_ctx_t *ctx=turbo_script_init(TURBO_SCRIPT_INIT_BARE); check_not_null(ctx); if(!ctx)return;
    check_equal(turbo_script_run(ctx,"id = timer.every(1000, () => 1); timer.cancel(id);"),0);
    wait_for_timers(ctx); check_equal(turbo_script_timer_active_count(ctx),(size_t)0); turbo_script_free(ctx);
  }
}
