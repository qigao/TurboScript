#include "ts_plugin.h"
#include "exprtk.h"
#include "exprtk_class.h"
#include "turbo_script_class_databind.h"

#include <orm.h>
#include <orm_runtime.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_MAX_DRIVERS 16u
#define DB_MAX_CONNECTIONS 64u
#define DB_MAX_OPTIONS 64u

typedef struct db_driver_binding_s {
  char *id;
  size_t id_len;
  char *module_path;
  size_t module_path_len;
} db_driver_binding_t;

typedef struct db_row_plan_entry_s {
  char *stable_id;
  DataBind *codec;
  DataBindMessagePlan *plan;
  const cmeta_data_desc *data;
  struct db_row_plan_entry_s *next;
} db_row_plan_entry_t;

typedef struct db_row_factory_context_s {
  struct db_ctx_s *db;
  exprtk_class_t *klass;
} db_row_factory_context_t;

typedef struct db_ctx_s {
  orm_runtime_t *runtime;
  exprtk_env_t *env;
  mem_pool_t *scratch;
  db_driver_binding_t drivers[DB_MAX_DRIVERS];
  size_t driver_count;
  orm_connection_t *connections[DB_MAX_CONNECTIONS];
  db_row_plan_entry_t *row_plans;
} db_ctx_t;

typedef struct db_connect_request_s {
  orm_string_view_t driver;
  orm_string_view_t module_path;
  orm_option_t *options;
  uint32_t option_count;
} db_connect_request_t;

static char *db_copy_view(orm_string_view_t value) {
  char *copy;
  if (!value.data || value.len == 0u ||
      memchr(value.data, '\0', value.len) != NULL)
    return NULL;
  copy = (char *)malloc(value.len + 1u);
  if (!copy) return NULL;
  memcpy(copy, value.data, value.len);
  copy[value.len] = '\0';
  return copy;
}

static exprtk_value_t db_fail(db_ctx_t *ctx, const char *role,
                              const orm_error_t *error) {
  const char *detail = error && error->message[0] ? error->message : NULL;
  if (ctx && ctx->env) {
    ctx->env->aborted = 1;
    snprintf(ctx->env->error_msg, sizeof(ctx->env->error_msg),
             "%s%s%s",
             role ? role : "database operation failed",
             detail ? ": " : "",
             detail ? detail : "");
  }
  return exprtk_val_num(0.0);
}

static db_row_plan_entry_t *db_row_plan_find(
    db_ctx_t *ctx, const cmeta_data_desc *data) {
  db_row_plan_entry_t *entry = ctx ? ctx->row_plans : NULL;
  while (entry) {
    if (entry->plan && entry->codec && entry->stable_id &&
        data && data->stable_id &&
        strcmp(entry->stable_id, data->stable_id) == 0 &&
        cmeta_data_desc_equal(entry->data, data) &&
        cmeta_data_desc_equal(
            data_bind_message_plan_object_data(entry->plan), data))
      return entry;
    entry = entry->next;
  }
  return NULL;
}

static int db_row_plan_get(
    db_ctx_t *ctx, exprtk_class_t *klass,
    db_row_plan_entry_t **out, char *error, size_t error_len) {
  const cmeta_data_desc *data;
  db_row_plan_entry_t *entry;
  char compile_error[512] = {0};
  int status;

  if (out) *out = NULL;
  if (!ctx || !klass || !out ||
      !exprtk_class_finalize_cmeta_data(klass)) {
    if (error && error_len)
      snprintf(error, error_len, "invalid typed RowClass reflection");
    return 0;
  }

  data = exprtk_class_cmeta_data(klass);
  if (!data || !data->stable_id) {
    if (error && error_len)
      snprintf(error, error_len, "typed RowClass has no canonical CMeta data identity");
    return 0;
  }

  entry = db_row_plan_find(ctx, data);
  if (entry) {
    *out = entry;
    return 1;
  }

  entry = (db_row_plan_entry_t *)calloc(1u, sizeof(*entry));
  if (!entry) {
    if (error && error_len)
      snprintf(error, error_len, "out of memory caching typed RowClass plan");
    return 0;
  }

  status = turbo_script_class_databind_compile(
      klass, &entry->codec, &entry->plan,
      compile_error, sizeof(compile_error));
  if (status <= 0) {
    free(entry);
    if (error && error_len) {
      snprintf(
          error, error_len, "%s",
          status < 0
              ? "RowClass contains a field outside the typed DataBind domain"
              : (compile_error[0]
                     ? compile_error
                     : "typed RowClass DataBind compilation failed"));
    }
    return 0;
  }

  entry->data = data;
  {
    const size_t stable_len = strlen(data->stable_id);
    entry->stable_id = (char *)malloc(stable_len + 1u);
    if (entry->stable_id)
      memcpy(entry->stable_id, data->stable_id, stable_len + 1u);
  }
  if (!entry->stable_id) {
    data_bind_message_plan_free(entry->plan);
    data_bind_free(entry->codec);
    free(entry);
    if (error && error_len)
      snprintf(error, error_len, "out of memory caching RowClass identity");
    return 0;
  }

  entry->next = ctx->row_plans;
  ctx->row_plans = entry;
  *out = entry;
  return 1;
}

static void db_row_plan_cache_destroy(db_ctx_t *ctx) {
  db_row_plan_entry_t *entry;
  if (!ctx) return;
  entry = ctx->row_plans;
  ctx->row_plans = NULL;
  while (entry) {
    db_row_plan_entry_t *next = entry->next;
    data_bind_message_plan_free(entry->plan);
    data_bind_free(entry->codec);
    free(entry->stable_id);
    free(entry);
    entry = next;
  }
}

static orm_status_t db_row_factory_create(
    void *context, void *out_value, cmeta_object_ref *out_object,
    orm_error_t *error) {
  db_row_factory_context_t *factory =
      (db_row_factory_context_t *)context;
  exprtk_value_t *carrier = (exprtk_value_t *)out_value;
  exprtk_instance_t *instance;
  cmeta_status cmeta_status_;

  if (error) orm_error_init(error);
  if (!factory || !factory->db || !factory->db->env ||
      !factory->klass || !carrier || !out_object) {
    if (error) {
      error->status = ORM_STATUS_INVALID_ARGUMENT;
      snprintf(error->message, sizeof(error->message),
               "invalid TurboScript typed row factory");
    }
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  memset(carrier, 0, sizeof(*carrier));
  carrier->type = EXPRTK_VAL_NULL;
  instance = exprtk_instance_create(
      factory->klass, &factory->db->env->arena);
  if (!instance) {
    if (error) {
      error->status = ORM_STATUS_OUT_OF_MEMORY;
      snprintf(error->message, sizeof(error->message),
               "allocate TurboScript typed row instance");
    }
    return ORM_STATUS_OUT_OF_MEMORY;
  }

  *carrier = exprtk_val_instance(instance);
  cmeta_status_ = exprtk_instance_borrow_cmeta_object(instance, out_object);
  if (cmeta_status_ != CMETA_OK) {
    exprtk_instance_destroy(instance);
    memset(carrier, 0, sizeof(*carrier));
    carrier->type = EXPRTK_VAL_NULL;
    if (error) {
      error->status = ORM_STATUS_TYPE_ERROR;
      snprintf(error->message, sizeof(error->message),
               "borrow typed row as CMeta object failed (%d)",
               (int)cmeta_status_);
    }
    return ORM_STATUS_TYPE_ERROR;
  }

  return ORM_STATUS_OK;
}

static void db_row_factory_destroy(void *context, void *value) {
  exprtk_value_t *carrier = (exprtk_value_t *)value;
  (void)context;
  if (!carrier) return;
  if (carrier->type == EXPRTK_VAL_INSTANCE &&
      carrier->data.instance_val.instance) {
    exprtk_instance_destroy(carrier->data.instance_val.instance);
  }
  memset(carrier, 0, sizeof(*carrier));
  carrier->type = EXPRTK_VAL_NULL;
}

static void db_row_list_rollback(exprtk_value_t *list) {
  if (!list || list->type != EXPRTK_VAL_LIST) return;
  for (size_t i = 0u; i < list->data.list.count; ++i) {
    exprtk_value_t *item = &list->data.list.items[i];
    if (item->type == EXPRTK_VAL_INSTANCE &&
        item->data.instance_val.instance)
      exprtk_instance_destroy(item->data.instance_val.instance);
    memset(item, 0, sizeof(*item));
    item->type = EXPRTK_VAL_NULL;
  }
  exprtk_value_destroy(list);
}

static int db_string_field(const exprtk_value_t *map, const char *name,
                           orm_string_view_t *out) {
  exprtk_value_t value;
  if (!map || !name || !out || !exprtk_map_has(map, name)) return 0;
  value = exprtk_map_get(map, name);
  if (value.type != EXPRTK_VAL_STRING || value.data.string.len == 0u ||
      memchr(value.data.string.data, '\0', value.data.string.len) != NULL)
    return 0;
  *out = value.data.string;
  return 1;
}

static int db_parse_options(db_ctx_t *ctx, const exprtk_value_t *config,
                            orm_option_t **out_options,
                            uint32_t *out_count) {
  exprtk_value_t options;
  exprtk_map_iter_t it;
  const char *key = NULL;
  exprtk_value_t value;
  size_t count = 0u;
  orm_option_t *items;

  *out_options = NULL;
  *out_count = 0u;
  if (!exprtk_map_has(config, "options")) return 1;

  options = exprtk_map_get(config, "options");
  if (options.type != EXPRTK_VAL_MAP) return 0;

  it = exprtk_map_iter_begin(&options);
  while (exprtk_map_iter_next(&it, &key, &value)) {
    if (!key || key[0] == '\0' || value.type != EXPRTK_VAL_STRING)
      return 0;
    if (++count > DB_MAX_OPTIONS) return 0;
  }
  if (count == 0u) return 1;

  items = (orm_option_t *)mem_alloc(ctx->scratch, count * sizeof(*items));
  if (!items) return 0;

  it = exprtk_map_iter_begin(&options);
  count = 0u;
  while (exprtk_map_iter_next(&it, &key, &value)) {
    items[count].keyword = orm_view(key);
    items[count].value = value.data.string;
    ++count;
  }

  *out_options = items;
  *out_count = (uint32_t)count;
  return 1;
}

static int db_parse_connect_request(db_ctx_t *ctx,
                                    const exprtk_value_t *config,
                                    db_connect_request_t *out) {
  exprtk_map_iter_t it;
  const char *key = NULL;
  exprtk_value_t ignored;

  if (!ctx || !config || !out || config->type != EXPRTK_VAL_MAP)
    return 0;
  memset(out, 0, sizeof(*out));

  it = exprtk_map_iter_begin(config);
  while (exprtk_map_iter_next(&it, &key, &ignored)) {
    if (!key ||
        (strcmp(key, "driver") != 0 &&
         strcmp(key, "module_path") != 0 &&
         strcmp(key, "options") != 0))
      return 0;
  }

  if (!db_string_field(config, "driver", &out->driver) ||
      !db_string_field(config, "module_path", &out->module_path))
    return 0;

  return db_parse_options(ctx, config, &out->options, &out->option_count);
}

static int db_view_equal(const char *text, size_t text_len,
                         orm_string_view_t value) {
  return text && text_len == value.len &&
         (text_len == 0u || memcmp(text, value.data, text_len) == 0);
}

static db_driver_binding_t *db_find_driver(
    db_ctx_t *ctx, orm_string_view_t driver) {
  if (!ctx) return NULL;
  for (size_t i = 0u; i < ctx->driver_count; ++i) {
    if (db_view_equal(ctx->drivers[i].id, ctx->drivers[i].id_len, driver))
      return &ctx->drivers[i];
  }
  return NULL;
}

static int db_ensure_driver(db_ctx_t *ctx,
                            const db_connect_request_t *request,
                            orm_error_t *error) {
  db_driver_binding_t *existing;
  orm_driver_load_config_t load = {0};
  char *id_copy = NULL;
  char *path_copy = NULL;
  orm_status_t status;

  existing = db_find_driver(ctx, request->driver);
  if (existing) {
    if (!db_view_equal(existing->module_path, existing->module_path_len,
                       request->module_path)) {
      orm_error_init(error);
      error->status = ORM_STATUS_INVALID_ARGUMENT;
      snprintf(error->message, sizeof(error->message),
               "driver '%s' was already loaded from a different module_path",
               existing->id);
      return 0;
    }
    return 1;
  }

  if (ctx->driver_count >= DB_MAX_DRIVERS) {
    orm_error_init(error);
    error->status = ORM_STATUS_LIMIT_EXCEEDED;
    snprintf(error->message, sizeof(error->message),
             "TurboScript db driver cache is full");
    return 0;
  }

  id_copy = db_copy_view(request->driver);
  path_copy = db_copy_view(request->module_path);
  if (!id_copy || !path_copy) {
    free(id_copy);
    free(path_copy);
    orm_error_init(error);
    error->status = ORM_STATUS_OUT_OF_MEMORY;
    snprintf(error->message, sizeof(error->message),
             "copy explicit database driver configuration");
    return 0;
  }

  load.struct_size = sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = request->module_path;
  load.expected_driver_id = request->driver;

  status = orm_runtime_load_driver(ctx->runtime, &load, error);
  if (status != ORM_STATUS_OK) {
    free(id_copy);
    free(path_copy);
    return 0;
  }

  ctx->drivers[ctx->driver_count++] = (db_driver_binding_t){
      id_copy, request->driver.len,
      path_copy, request->module_path.len};
  return 1;
}

static int db_connection_slot(db_ctx_t *ctx) {
  for (size_t i = 0u; i < DB_MAX_CONNECTIONS; ++i) {
    if (!ctx->connections[i]) return (int)i;
  }
  return -1;
}

static int db_handle_value(exprtk_value_t value, size_t *out_index) {
  int64_t handle;
  if (!out_index) return 0;

  if (value.type == EXPRTK_VAL_INTEGER) {
    handle = value.data.integer;
  } else if (value.type == EXPRTK_VAL_NUMBER &&
             isfinite(value.data.number) &&
             floor(value.data.number) == value.data.number &&
             value.data.number >= 1.0 &&
             value.data.number <= (double)DB_MAX_CONNECTIONS) {
    handle = (int64_t)value.data.number;
  } else {
    return 0;
  }

  if (handle < 1 || handle > (int64_t)DB_MAX_CONNECTIONS) return 0;
  *out_index = (size_t)(handle - 1);
  return 1;
}

static exprtk_value_t db_connect(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, void *user_data) {
  db_ctx_t *ctx = (db_ctx_t *)user_data;
  db_connect_request_t request;
  orm_config_t config;
  orm_connection_t *connection = NULL;
  orm_error_t error;
  orm_status_t status;
  int slot;
  (void)env;

  if (!ctx || argc != 1u ||
      !db_parse_connect_request(ctx, &args[0], &request)) {
    return db_fail(ctx,
                   "db.connect requires {driver,module_path,options?}",
                   NULL);
  }

  orm_error_init(&error);
  if (!db_ensure_driver(ctx, &request, &error))
    return db_fail(ctx, "db.connect driver load failed", &error);

  slot = db_connection_slot(ctx);
  if (slot < 0) {
    orm_error_init(&error);
    error.status = ORM_STATUS_LIMIT_EXCEEDED;
    snprintf(error.message, sizeof(error.message),
             "TurboScript db connection table is full");
    return db_fail(ctx, "db.connect failed", &error);
  }

  orm_config(&config);
  config.driver = request.driver;
  config.options = request.options;
  config.option_count = request.option_count;

  orm_error_init(&error);
  status = orm_runtime_connect(ctx->runtime, &config, &connection, &error);
  if (status != ORM_STATUS_OK || !connection)
    return db_fail(ctx, "db.connect failed", &error);

  ctx->connections[slot] = connection;
  return exprtk_val_int((int64_t)slot + 1);
}

static int db_orm_value(exprtk_value_t input, orm_value_t *out) {
  if (!out) return 0;

  switch (input.type) {
    case EXPRTK_VAL_NULL:
      *out = orm_null();
      return 1;
    case EXPRTK_VAL_BOOL:
      *out = orm_bool(input.data.boolean != 0);
      return 1;
    case EXPRTK_VAL_INTEGER:
      *out = orm_i64(input.data.integer);
      return 1;
    case EXPRTK_VAL_NUMBER:
      if (!isfinite(input.data.number)) return 0;
      *out = orm_f64(input.data.number);
      return 1;
    case EXPRTK_VAL_STRING:
      *out = orm_text_v(input.data.string);
      return 1;
    case EXPRTK_VAL_BYTES:
      *out = orm_blob(input.data.bytes.data, input.data.bytes.len);
      return 1;
    default:
      return 0;
  }
}

static int db_bind_params(orm_query_t *query, const exprtk_value_t *params,
                          orm_error_t *error) {
  orm_status_t status;

  if (!params) return 1;
  if (!query || params->type != EXPRTK_VAL_LIST) return 0;

  for (size_t i = 0u; i < params->data.list.count; ++i) {
    orm_value_t value;
    if (!db_orm_value(params->data.list.items[i], &value)) return 0;
    status = orm_query_bind(query, value, error);
    if (status != ORM_STATUS_OK) return 0;
  }
  return 1;
}

static exprtk_value_t db_exec(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, void *user_data) {
  db_ctx_t *ctx = (db_ctx_t *)user_data;
  size_t slot;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  cflow_publisher publisher = {0};
  orm_command_result_t command_result = ORM_COMMAND_RESULT_INIT;
  orm_error_t error;
  orm_status_t status;
  cflow_step step;
  exprtk_value_t result;
  (void)env;

  if (!ctx || (argc != 2u && argc != 3u) ||
      !db_handle_value(args[0], &slot) ||
      !(connection = ctx->connections[slot]) ||
      args[1].type != EXPRTK_VAL_STRING) {
    return db_fail(
        ctx,
        "db.exec requires (connection, sql [, scalar_params_list])",
        NULL);
  }

  orm_error_init(&error);
  status = orm_raw(connection, args[1].data.string, &query, &error);
  if (status != ORM_STATUS_OK || !query)
    return db_fail(ctx, "db.exec query creation failed", &error);

  if (argc == 3u && !db_bind_params(query, &args[2], &error)) {
    orm_query_destroy(query);
    return db_fail(
        ctx,
        error.status != ORM_STATUS_OK
            ? "db.exec parameter binding failed"
            : "db.exec params must be a list of null/bool/int/number/string/bytes",
        error.status != ORM_STATUS_OK ? &error : NULL);
  }

  status = orm_query_open_command_flow(query, &publisher, &error);
  if (status != ORM_STATUS_OK || !cflow_publisher_valid(&publisher)) {
    orm_query_destroy(query);
    return db_fail(ctx, "db.exec command Publisher open failed", &error);
  }

  step = cflow_publisher_resume(&publisher, NULL, &command_result);
  if (step.kind != CFLOW_STEP_VALUE_AND_DONE) {
    char message[256];
    snprintf(
        message, sizeof(message), "%s",
        step.error && step.error[0]
            ? step.error
            : "command Publisher did not produce VALUE_AND_DONE");
    cflow_publisher_destroy(&publisher);
    orm_query_destroy(query);
    if (ctx && ctx->env) {
      ctx->env->aborted = 1;
      snprintf(ctx->env->error_msg, sizeof(ctx->env->error_msg),
               "db.exec command failed: %s", message);
    }
    return exprtk_val_num(0.0);
  }

  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);

  if (command_result.affected_rows > (uint64_t)INT64_MAX)
    return db_fail(ctx, "db.exec affected_rows exceeds script int64 range", NULL);

  result = exprtk_val_int((int64_t)command_result.affected_rows);
  return result;
}

static exprtk_value_t db_query(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, void *user_data) {
  db_ctx_t *ctx = (db_ctx_t *)user_data;
  size_t slot;
  orm_connection_t *connection;
  exprtk_class_t *klass;
  db_row_plan_entry_t *plan_entry = NULL;
  db_row_factory_context_t factory_context;
  orm_object_row_factory_t factory;
  orm_object_flow_config_t flow;
  orm_query_t *query = NULL;
  cflow_publisher publisher = {0};
  orm_error_t error;
  orm_status_t status;
  exprtk_value_t rows = exprtk_val_list_empty();
  char plan_error[512] = {0};
  int publisher_open = 0;
  (void)env;

  if (!ctx || (argc != 3u && argc != 4u) ||
      !db_handle_value(args[0], &slot) ||
      !(connection = ctx->connections[slot]) ||
      args[1].type != EXPRTK_VAL_STRING ||
      args[2].type != EXPRTK_VAL_CLASS ||
      !(klass = args[2].data.class_val.klass) ||
      exprtk_class_is_abstract(klass) ||
      exprtk_class_is_interface(klass)) {
    exprtk_value_destroy(&rows);
    return db_fail(
        ctx,
        "db.query requires (connection, sql, concrete RowClass [, scalar_params_list])",
        NULL);
  }

  if (!db_row_plan_get(
          ctx, klass, &plan_entry, plan_error, sizeof(plan_error))) {
    exprtk_value_destroy(&rows);
    if (ctx && ctx->env) {
      ctx->env->aborted = 1;
      snprintf(ctx->env->error_msg, sizeof(ctx->env->error_msg),
               "db.query RowClass binding failed: %s",
               plan_error[0] ? plan_error : "invalid typed class");
    }
    return exprtk_val_num(0.0);
  }

  orm_error_init(&error);
  status = orm_raw(connection, args[1].data.string, &query, &error);
  if (status != ORM_STATUS_OK || !query) {
    exprtk_value_destroy(&rows);
    return db_fail(ctx, "db.query creation failed", &error);
  }

  if (argc == 4u && !db_bind_params(query, &args[3], &error)) {
    orm_query_destroy(query);
    exprtk_value_destroy(&rows);
    return db_fail(
        ctx,
        error.status != ORM_STATUS_OK
            ? "db.query parameter binding failed"
            : "db.query params must be a list of null/bool/int/number/string/bytes",
        error.status != ORM_STATUS_OK ? &error : NULL);
  }

  factory_context = (db_row_factory_context_t){ctx, klass};
  factory = (orm_object_row_factory_t){
      sizeof(orm_object_row_factory_t),
      ORM_C_ABI_VERSION,
      exprtk_value_cmeta_type(),
      &factory_context,
      db_row_factory_create,
      db_row_factory_destroy
  };
  orm_object_flow_config(
      &flow, plan_entry->data, plan_entry->plan, &factory);
  if (ctx->env->max_external_value_bytes != 0u &&
      ctx->env->max_external_value_bytes < flow.max_buffer_bytes)
    flow.max_buffer_bytes = ctx->env->max_external_value_bytes;

  orm_error_init(&error);
  status = orm_query_open_object_flow(query, &flow, &publisher, &error);
  if (status != ORM_STATUS_OK || !cflow_publisher_valid(&publisher)) {
    orm_query_destroy(query);
    exprtk_value_destroy(&rows);
    return db_fail(ctx, "db.query object Publisher open failed", &error);
  }
  publisher_open = 1;

  for (;;) {
    exprtk_value_t row = {.type = EXPRTK_VAL_NULL};
    cflow_step step = cflow_publisher_resume(&publisher, NULL, &row);

    if (step.kind == CFLOW_STEP_DONE) break;

    if (step.kind == CFLOW_STEP_WAIT) {
      if (publisher_open) cflow_publisher_destroy(&publisher);
      orm_query_destroy(query);
      db_row_list_rollback(&rows);
      return db_fail(
          ctx,
          "db.query synchronous object flow cannot consume WAIT; async query API is required",
          NULL);
    }

    if (step.kind == CFLOW_STEP_ERROR) {
      char message[512];
      snprintf(
          message, sizeof(message), "%s",
          step.error && step.error[0]
              ? step.error
              : "typed row Publisher failed");
      if (publisher_open) cflow_publisher_destroy(&publisher);
      orm_query_destroy(query);
      db_row_list_rollback(&rows);
      if (ctx && ctx->env) {
        ctx->env->aborted = 1;
        snprintf(ctx->env->error_msg, sizeof(ctx->env->error_msg),
                 "db.query failed: %s", message);
      }
      return exprtk_val_num(0.0);
    }

    if (step.kind != CFLOW_STEP_VALUE &&
        step.kind != CFLOW_STEP_VALUE_AND_DONE) {
      if (row.type == EXPRTK_VAL_INSTANCE)
        db_row_factory_destroy(&factory_context, &row);
      if (publisher_open) cflow_publisher_destroy(&publisher);
      orm_query_destroy(query);
      db_row_list_rollback(&rows);
      return db_fail(ctx, "db.query Publisher returned an invalid step", NULL);
    }

    if (row.type != EXPRTK_VAL_INSTANCE ||
        !row.data.instance_val.instance ||
        !exprtk_instance_of(row.data.instance_val.instance, klass)) {
      db_row_factory_destroy(&factory_context, &row);
      if (publisher_open) cflow_publisher_destroy(&publisher);
      orm_query_destroy(query);
      db_row_list_rollback(&rows);
      return db_fail(
          ctx, "db.query Publisher returned an incompatible RowClass value", NULL);
    }

    if (exprtk_list_push(&rows, row) != 0) {
      db_row_factory_destroy(&factory_context, &row);
      if (publisher_open) cflow_publisher_destroy(&publisher);
      orm_query_destroy(query);
      db_row_list_rollback(&rows);
      return db_fail(ctx, "db.query could not append typed row", NULL);
    }

    /* Ownership of the instance now lives through the result list / env graph.
     * The local carrier itself owns no separate reference. */
    memset(&row, 0, sizeof(row));
    row.type = EXPRTK_VAL_NULL;

    if (step.kind == CFLOW_STEP_VALUE_AND_DONE) break;
  }

  cflow_publisher_destroy(&publisher);
  publisher_open = 0;
  orm_query_destroy(query);
  return rows;
}

static exprtk_value_t db_close(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, void *user_data) {
  db_ctx_t *ctx = (db_ctx_t *)user_data;
  size_t slot;
  orm_error_t error;
  orm_status_t status;
  orm_connection_t *connection;
  (void)env;

  if (!ctx || argc != 1u || !db_handle_value(args[0], &slot) ||
      !(connection = ctx->connections[slot])) {
    return db_fail(ctx, "db.close received an invalid connection handle", NULL);
  }

  orm_error_init(&error);
  status = orm_connection_close(connection, &error);
  if (status != ORM_STATUS_OK)
    return db_fail(ctx, "db.close failed", &error);

  orm_connection_release(connection);
  ctx->connections[slot] = NULL;
  return exprtk_val_bool(1);
}

static void db_ctx_destroy(db_ctx_t *ctx) {
  orm_error_t error;
  if (!ctx) return;

  for (size_t i = 0u; i < DB_MAX_CONNECTIONS; ++i) {
    if (ctx->connections[i]) {
      orm_error_init(&error);
      (void)orm_connection_close(ctx->connections[i], &error);
      orm_connection_release(ctx->connections[i]);
      ctx->connections[i] = NULL;
    }
  }

  db_row_plan_cache_destroy(ctx);

  if (ctx->runtime) {
    orm_error_init(&error);
    (void)orm_runtime_close(ctx->runtime, &error);
    orm_runtime_release(ctx->runtime);
    ctx->runtime = NULL;
  }

  for (size_t i = 0u; i < ctx->driver_count; ++i) {
    free(ctx->drivers[i].id);
    free(ctx->drivers[i].module_path);
  }
  free(ctx);
}

static void *db_module_load(void *self, void *env_ptr, void *scratch_ptr) {
  db_ctx_t *ctx;
  orm_runtime_config_t runtime_config;
  orm_error_t error;
  orm_status_t status;
  exprtk_env_t *env = (exprtk_env_t *)env_ptr;
  mem_pool_t *scratch = (mem_pool_t *)scratch_ptr;
  (void)self;

  if (!env || !scratch) return NULL;

  ctx = (db_ctx_t *)calloc(1u, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->env = env;
  ctx->scratch = scratch;

  orm_runtime_config_init(&runtime_config);
  orm_error_init(&error);
  status = orm_runtime_create(&runtime_config, &ctx->runtime, &error);
  if (status != ORM_STATUS_OK || !ctx->runtime) {
    free(ctx);
    return NULL;
  }

  exprtk_env_register_func(env, "db.connect", db_connect, ctx);
  exprtk_env_register_func(env, "db.exec", db_exec, ctx);
  exprtk_env_register_func(env, "db.query", db_query, ctx);
  exprtk_env_register_func(env, "db.close", db_close, ctx);
  return ctx;
}

static void db_module_unload(void *self, void *instance) {
  (void)self;
  db_ctx_destroy((db_ctx_t *)instance);
}

TS_PLUGIN_DETAIL_EXPORT(db, db_module_load, db_module_unload)
