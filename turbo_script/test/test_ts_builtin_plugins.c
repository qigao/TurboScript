#include "tinytest.h"
#include "turbo_script.h"

spec("built-in Salts Plugin modules") {
  static turbo_script_ctx_t *first;
  static turbo_script_ctx_t *second;
  before_each() {
    first = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    second = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
  }
  after_each() {
    if (first) turbo_script_free(first);
    if (second) turbo_script_free(second);
    first = second = NULL;
  }

  it("opens every real plugin in independent contexts and preserves the surviving context") {
    const char *names[] = {
        "net", "ta", "fin", "ts", "sqlite", "mapper", "img", "crypto", "hash", "fuzzy", "os"
    };
    check_not_null(first);
    check_not_null(second);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
      check_equal(turbo_script_load_plugin(first, names[i]), 0);
      check_equal(turbo_script_load_plugin(second, names[i]), 0);
      /* Re-import must reuse the instance and its lease. */
      check_equal(turbo_script_load_plugin(second, names[i]), 0);
    }
    turbo_script_free(first);
    first = NULL;
    /* Offline call through the remaining net DSO's registered script function. */
    check_equal(turbo_script_run(second, "import(\"net\"); ws.close();"), 0);
  }
}
