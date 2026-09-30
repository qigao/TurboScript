#include "../src/turbo_script_internal.h"
#include "sqlite_provider.h"
#include "tinytest.h"

#include <math.h>
#include <string.h>

static const salts_plugin_export *sqlite_provider_export_from_ctx(
    turbo_script_ctx_t *ctx) {
  const salts_plugin_export *found = NULL;
  if (!ctx) return NULL;

  for (size_t i = 0u; i < ctx->plugin_count; ++i) {
    ts_plugin_handle_t *handle = ctx->plugins[i];
    if (!handle || !handle->manifest ||
        strcmp(handle->manifest->plugin_id, "sqlite") != 0)
      continue;
    if (salts_plugin_manifest_find_export(
            handle->manifest, SQLITE_PROVIDER_EXPORT_ID, &found) ==
        SALTS_PLUGIN_OK)
      return found;
  }
  return NULL;
}

spec("TurboScript SQLite canonical provider") {
  it("publishes a valid provider Interface under the retained plugin lease") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    const salts_plugin_export *entry;
    sqlite_provider *provider;

    check_not_null(ctx);
    check_equal(turbo_script_load_plugin(ctx, "sqlite"), 0);

    entry = sqlite_provider_export_from_ctx(ctx);
    check_not_null(entry);
    if (entry) {
      check_equal(entry->kind, SALTS_PLUGIN_EXPORT_INTERFACE);
      check_equal(
          salts_plugin_export_require_interface(
              entry, SQLITE_PROVIDER_CONTRACT_ID,
              SQLITE_PROVIDER_CONTRACT_VERSION, 0u,
              sqlite_provider_interface()),
          SALTS_PLUGIN_OK);
      provider = (sqlite_provider *)entry->value.interface.value;
      check_true(sqlite_provider_valid(provider));
      check_equal(provider->vtable->implementation, "sqlite_provider_impl");
      check_equal(sqlite_provider_interface()->method_count, (size_t)9u);
    }

    turbo_script_free(ctx);
  }

  it("creates isolated provider sessions inside one plugin lease") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    const salts_plugin_export *entry;
    sqlite_provider *provider;
    void *left;
    void *right;
    int left_db;
    int right_db;
    double left_tables = -1.0;
    double right_tables = -1.0;

    check_not_null(ctx);
    check_equal(turbo_script_load_plugin(ctx, "sqlite"), 0);
    entry = sqlite_provider_export_from_ctx(ctx);
    check_not_null(entry);
    provider = entry ? (sqlite_provider *)entry->value.interface.value : NULL;
    check_true(sqlite_provider_valid(provider));

    left = sqlite_provider_create(provider);
    right = sqlite_provider_create(provider);
    check_not_null(left);
    check_not_null(right);
    check(left != right);

    left_db = sqlite_provider_open(provider, left, ":memory:");
    right_db = sqlite_provider_open(provider, right, ":memory:");
    check(left_db >= 0);
    check(right_db >= 0);

    check(sqlite_provider_exec(
              provider, left, left_db,
              "CREATE TABLE isolated(value REAL);"
              "INSERT INTO isolated(value) VALUES (1.0);") >= 0);

    check_true(sqlite_provider_query_scalar(
        provider, left, left_db,
        "SELECT COUNT(*) FROM sqlite_master WHERE name='isolated';",
        &left_tables));
    check_true(sqlite_provider_query_scalar(
        provider, right, right_db,
        "SELECT COUNT(*) FROM sqlite_master WHERE name='isolated';",
        &right_tables));
    check(fabs(left_tables - 1.0) <= 1e-9);
    check(fabs(right_tables - 0.0) <= 1e-9);

    sqlite_provider_close(provider, left, left_db);
    sqlite_provider_close(provider, right, right_db);
    sqlite_provider_destroy(provider, left);
    sqlite_provider_destroy(provider, right);
    turbo_script_free(ctx);
  }

  it("owns query-column storage until explicit provider release") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    const salts_plugin_export *entry;
    sqlite_provider *provider;
    void *session;
    int db;
    sqlite_provider_column_request request = {
        "SELECT 10.0 AS a, 20.0 AS b "
        "UNION ALL SELECT 30.0, 40.0;",
        1,
        8u
    };
    sqlite_provider_f64_column column = {0};

    check_not_null(ctx);
    check_equal(turbo_script_load_plugin(ctx, "sqlite"), 0);
    entry = sqlite_provider_export_from_ctx(ctx);
    provider = entry ? (sqlite_provider *)entry->value.interface.value : NULL;
    check_true(sqlite_provider_valid(provider));

    session = sqlite_provider_create(provider);
    check_not_null(session);
    db = sqlite_provider_open(provider, session, ":memory:");
    check(db >= 0);

    check_true(sqlite_provider_query_column(
        provider, session, db, &request, &column));
    check_equal(column.count, (size_t)2u);
    check_not_null(column.data);
    check(fabs(column.data[0] - 20.0) <= 1e-9);
    check(fabs(column.data[1] - 40.0) <= 1e-9);

    sqlite_provider_release_column(provider, session, &column);
    check_null(column.data);
    check_equal(column.count, (size_t)0u);

    sqlite_provider_close(provider, session, db);
    sqlite_provider_destroy(provider, session);
    turbo_script_free(ctx);
  }

  it("keeps the sqlite language facade aligned in interpreter and JIT") {
    const char *source =
        "db=sqlite.open(\":memory:\");"
        "changes=sqlite.exec(db,"
        "\"CREATE TABLE t(v REAL);"
        "INSERT INTO t(v) VALUES (1.5),(2.5),(3.5);\");"
        "scalar=sqlite.query_scalar(db,\"SELECT SUM(v) FROM t;\");"
        "column=sqlite.query_col(db,\"SELECT v FROM t ORDER BY v;\");"
        "sqlite.close(db);";
    turbo_script_ctx_t *interp = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    turbo_script_ctx_t *jit = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_value_t interp_column;
    exprtk_value_t jit_column;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(turbo_script_load_plugin(interp, "sqlite"), 0);
    check_equal(turbo_script_load_plugin(jit, "sqlite"), 0);

    check_equal(turbo_script_run(interp, source), 0);
    check_equal(turbo_script_run_jit(jit, source), 0);
    check(fabs(ts_get_num(interp, "scalar") - 7.5) <= 1e-9);
    check(fabs(ts_get_num(jit, "scalar") - 7.5) <= 1e-9);

    interp_column = exprtk_env_get(&interp->env, "column");
    jit_column = exprtk_env_get(&jit->env, "column");
    check_equal(interp_column.type, EXPRTK_VAL_VECTOR);
    check_equal(jit_column.type, EXPRTK_VAL_VECTOR);
    check_equal(interp_column.data.vector.size, (size_t)3u);
    check_equal(jit_column.data.vector.size, (size_t)3u);
    check(fabs(interp_column.data.vector.data[0] - 1.5) <= 1e-9);
    check(fabs(interp_column.data.vector.data[2] - 3.5) <= 1e-9);
    check(fabs(jit_column.data.vector.data[0] - 1.5) <= 1e-9);
    check(fabs(jit_column.data.vector.data[2] - 3.5) <= 1e-9);

    turbo_script_free(jit);
    turbo_script_free(interp);
  }
}
