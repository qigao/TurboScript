/**
 * @file exprtk_mod_http.c
 * @brief HTTP and WebSocket module: http.* and ws.* for TurboScript.
 */
#include "net_ctx.h"
#include "turbo_script.h"
#include "exprtk_module.h"
#include <CoroNet.h>
#include <turbo_parser.h>
#include <turbo_str.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define NET_ONE ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0})

/* == Lifecycle ============================================================ */

void *net_ctx_create(void) {
  return calloc(1, sizeof(net_ctx_t));
}

void net_ctx_destroy(void *p) {
  net_ctx_t *ctx = (net_ctx_t *)p;
  size_t i;
  if (!ctx) return;
  if (ctx->client) turbo_http_destroy(ctx->client);
  if (ctx->ws_client) coro_socket_destroy(ctx->ws_client);
  for (i = 0; i < NET_WS_TASK_CONNECTION_CAPACITY; ++i) {
    if (ctx->ws_task_connections[i].socket)
      coro_socket_destroy(ctx->ws_task_connections[i].socket);
  }
  free(ctx);
}

/* == Helpers ============================================================== */

static exprtk_value_t net_null_value(void) {
  exprtk_value_t value;
  memset(&value, 0, sizeof(value));
  value.type = EXPRTK_VAL_NULL;
  return value;
}

static exprtk_value_t net_make_string_value(exprtk_env_t *env, const char *data, size_t len) {
  exprtk_value_t value;
  exprtk_value_t borrowed = exprtk_val_str(tstr_v_from_buf(data ? data : "", len));
  if (!env || (!data && len != 0)) return NET_ZERO;
  if (len > env->max_external_value_bytes) {
    env->aborted = 1;
    snprintf(env->error_msg, sizeof(env->error_msg),
             "network value size %zu exceeds quota %zu", len,
             env->max_external_value_bytes);
    return NET_ZERO;
  }
  if (exprtk_value_copy_to_env(borrowed, env, &value) != 0)
    return NET_ZERO;
  return value;
}

static exprtk_value_t copy_response_body(http_response_t *resp, exprtk_env_t *env) {
  if (resp && resp->status_code >= 200 && resp->status_code < 300 && resp->body) {
    return net_make_string_value(env, resp->body, resp->body_len);
  }
  return NET_ZERO;
}

static void net_set_error(net_ctx_t *ctx, const char *msg) {
  if (!ctx) return;
  snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg ? msg : "");
}

static int net_parse_ws_url(const char *url, const char **host, size_t *host_len,
                            int *port, const char **path, int *is_tls) {
  const char *scheme_end;
  const char *host_start;
  const char *host_end;
  const char *path_start;

  if (!url || !host || !host_len || !port || !path || !is_tls) return -1;

  scheme_end = strstr(url, "://");
  if (!scheme_end) return -1;

  if ((size_t)(scheme_end - url) == 2 && strncmp(url, "ws", 2) == 0) {
    *is_tls = 0;
    *port = 80;
  } else if ((size_t)(scheme_end - url) == 3 && strncmp(url, "wss", 3) == 0) {
    *is_tls = 1;
    *port = 443;
  } else {
    return -1;
  }

  host_start = scheme_end + 3;
  if (*host_start == '\0') return -1;

  path_start = strchr(host_start, '/');
  host_end = path_start ? path_start : host_start + strlen(host_start);
  if (host_end == host_start) return -1;

  if (*host_start == '[') {
    const char *ipv6_end = memchr(host_start, ']', (size_t)(host_end - host_start));
    if (!ipv6_end || ipv6_end + 1 < host_end && ipv6_end[1] != ':') return -1;
    *host = host_start + 1;
    *host_len = (size_t)(ipv6_end - (host_start + 1));
    if (*host_len == 0) return -1;
    if (ipv6_end + 1 < host_end) {
      *port = atoi(ipv6_end + 2);
      if (*port <= 0) return -1;
    }
  } else {
    const char *colon = memchr(host_start, ':', (size_t)(host_end - host_start));
    *host = host_start;
    *host_len = (size_t)((colon ? colon : host_end) - host_start);
    if (*host_len == 0) return -1;
    if (colon) {
      *port = atoi(colon + 1);
      if (*port <= 0) return -1;
    }
  }

  *path = path_start ? path_start : "/";
  return 0;
}

static coro_socket_t **net_ctx_ws_slot(net_ctx_t *ctx, const coro_cancel_token_t *owner,
                                       int create) {
  size_t i;
  net_ws_task_connection_t *free_slot = NULL;

  if (!ctx) return NULL;
  if (!owner) return &ctx->ws_client;

  for (i = 0; i < NET_WS_TASK_CONNECTION_CAPACITY; ++i) {
    net_ws_task_connection_t *entry = &ctx->ws_task_connections[i];
    if (entry->socket && entry->owner == owner) return &entry->socket;
    if (!entry->socket && !free_slot) free_slot = entry;
  }
  if (!create || !free_slot) return NULL;

  free_slot->owner = owner;
  ctx->ws_task_connection_count++;
  return &free_slot->socket;
}

static void net_ctx_clear_ws_slot(net_ctx_t *ctx, const coro_cancel_token_t *owner,
                                  coro_socket_t **slot) {
  size_t i;

  if (!ctx || !slot) return;
  if (*slot) coro_socket_destroy(*slot);
  *slot = NULL;
  if (!owner) return;

  for (i = 0; i < NET_WS_TASK_CONNECTION_CAPACITY; ++i) {
    net_ws_task_connection_t *entry = &ctx->ws_task_connections[i];
    if (&entry->socket != slot) continue;
    entry->owner = NULL;
    if (ctx->ws_task_connection_count > 0) ctx->ws_task_connection_count--;
    return;
  }
}

static coro_socket_t *net_ctx_recreate_ws_client(net_ctx_t *ctx,
                                                  const coro_cancel_token_t *owner) {
  coro_context_t *cctx;
  coro_socket_t **slot;

  if (!ctx) return NULL;
  slot = net_ctx_ws_slot(ctx, owner, 1);
  if (!slot) {
    net_set_error(ctx, "ws task connection capacity exhausted");
    return NULL;
  }
  if (*slot) net_ctx_clear_ws_slot(ctx, owner, slot);
  slot = net_ctx_ws_slot(ctx, owner, 1);
  if (!slot) return NULL;

  cctx = coro_context_current();
  if (!cctx) {
    net_set_error(ctx, "no coroutine context");
    net_ctx_clear_ws_slot(ctx, owner, slot);
    return NULL;
  }

  *slot = coro_socket_create_tcpv4(cctx);
  if (!*slot) net_ctx_clear_ws_slot(ctx, owner, slot);
  return *slot;
}

static coro_socket_t *net_ctx_current_ws_client(net_ctx_t *ctx,
                                                const coro_cancel_token_t *owner) {
  coro_socket_t **slot = net_ctx_ws_slot(ctx, owner, 0);
  return slot ? *slot : NULL;
}

static void net_ws_cancel_wait(void *arg) {
  coro_socket_t *socket = (coro_socket_t *)arg;
  if (socket) (void)coro_socket_interrupt_wait(socket, TURBO_ECANCELED);
}

static char *net_trim_ascii(char *text) {
  char *end;

  while (*text && isspace((unsigned char)*text)) text++;
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    *--end = '\0';
  }
  return text;
}

static int net_truthy_value(const exprtk_value_t *value, int *out) {
  if (!value || !out) return 0;

  switch (value->type) {
    case EXPRTK_VAL_NUMBER:
      *out = value->data.number != 0.0;
      return 1;
    case EXPRTK_VAL_INTEGER:
      *out = value->data.integer != 0;
      return 1;
    case EXPRTK_VAL_STRING: {
      const char *text = value->data.string.data;
      size_t len = value->data.string.len;
      if (len == 4 && tstr_ncasecmp(text, "true", len) == 0) {
        *out = 1;
        return 1;
      }
      if (len == 5 && tstr_ncasecmp(text, "false", len) == 0) {
        *out = 0;
        return 1;
      }
      if (len == 1 && text[0] == '1') {
        *out = 1;
        return 1;
      }
      if (len == 1 && text[0] == '0') {
        *out = 0;
        return 1;
      }
      return 0;
    }
    default:
      return 0;
  }
}

static exprtk_value_t net_json_to_exprtk_value(http_ud_t *ud, const json_value_t *value) {
  size_t i;

  if (!value) return net_null_value();

  switch (turbo_json_type(value)) {
    case TURBO_JSON_NULL:
      return net_null_value();
    case TURBO_JSON_BOOL:
      return exprtk_val_num(turbo_json_bool(value) ? 1.0 : 0.0);
    case TURBO_JSON_NUMBER:
      return exprtk_val_num(turbo_json_number(value));
    case TURBO_JSON_STRING:
      return net_make_string_value(ud->env, turbo_json_string(value), turbo_json_string_len(value));
    case TURBO_JSON_ARRAY: {
      exprtk_value_t list = exprtk_val_list_empty();
      size_t count = turbo_json_array_size(value);
      for (i = 0; i < count; i++) {
        exprtk_value_t item = net_json_to_exprtk_value(ud, turbo_json_array_get(value, i));
        (void)exprtk_list_push(&list, item);
        exprtk_value_destroy(&item);
      }
      return list;
    }
    case TURBO_JSON_OBJECT: {
      exprtk_value_t map = exprtk_val_map();
      size_t count = turbo_json_object_size(value);
      for (i = 0; i < count; i++) {
        const char *key = turbo_json_object_key(value, i);
        exprtk_value_t item = net_json_to_exprtk_value(ud, turbo_json_object_value(value, i));
        exprtk_map_set(&map, key ? key : "", item);
        exprtk_value_destroy(&item);
      }
      return map;
    }
    default:
      return net_null_value();
  }
}

static exprtk_value_t net_response_headers_map(http_ud_t *ud, http_response_t *resp) {
  exprtk_value_t headers = exprtk_val_map();
  char *buf;
  char *line;
  size_t i;

  if (!resp || !resp->headers || resp->headers_len == 0) return headers;

  buf = mem_alloc(ud->scratch, resp->headers_len + 1);
  if (!buf) return headers;

  memcpy(buf, resp->headers, resp->headers_len);
  buf[resp->headers_len] = '\0';
  line = buf;

  for (i = 0; i <= resp->headers_len; i++) {
    char *entry;
    char *colon;
    char *name;
    char *value;

    if (buf[i] != '\n' && buf[i] != '\0') continue;

    buf[i] = '\0';
    entry = net_trim_ascii(line);
    colon = strchr(entry, ':');
    if (colon) {
      *colon = '\0';
      name = net_trim_ascii(entry);
      value = net_trim_ascii(colon + 1);
      exprtk_value_t header_value = net_make_string_value(ud->env, value, strlen(value));
      exprtk_map_set(&headers, name, header_value);
      exprtk_value_destroy(&header_value);
    }
    line = &buf[i + 1];
  }

  return headers;
}

static exprtk_value_t net_response_data_value(http_ud_t *ud, http_response_t *resp) {
  exprtk_value_t value = net_null_value();
  json_value_t *json = NULL;

  if (!resp || !http_response_is_json(resp)) return value;

  json = http_response_parse_json(resp);
  if (!json) return value;

  value = net_json_to_exprtk_value(ud, json);
  turbo_free_json(&json);
  return value;
}

static exprtk_value_t net_error_response_value(http_ud_t *ud, const char *error) {
  exprtk_value_t result = exprtk_val_map();
  exprtk_value_t text;
  exprtk_value_t headers;

  exprtk_map_set(&result, "status", exprtk_val_num(0.0));
  text = net_make_string_value(ud->env, "", 0);
  exprtk_map_set(&result, "body", text);
  exprtk_value_destroy(&text);
  headers = exprtk_val_map();
  exprtk_map_set(&result, "headers", headers);
  exprtk_value_destroy(&headers);
  exprtk_map_set(&result, "data", net_null_value());
  if (error && error[0]) {
    text = net_make_string_value(ud->env, error, strlen(error));
    exprtk_map_set(&result, "error", text);
    exprtk_value_destroy(&text);
  } else {
    exprtk_map_set(&result, "error", net_null_value());
  }

  return result;
}

static exprtk_value_t net_response_value(http_ud_t *ud, http_response_t *resp) {
  exprtk_value_t result = exprtk_val_map();
  exprtk_value_t body;
  exprtk_value_t error;
  exprtk_value_t headers;
  exprtk_value_t data;

  exprtk_map_set(&result, "status", exprtk_val_num(resp ? (double)resp->status_code : 0.0));
  body = net_make_string_value(ud->env, resp && resp->body ? resp->body : "",
                               resp && resp->body ? resp->body_len : 0);
  exprtk_map_set(&result, "body", body);
  exprtk_value_destroy(&body);
  headers = net_response_headers_map(ud, resp);
  exprtk_map_set(&result, "headers", headers);
  exprtk_value_destroy(&headers);
  data = net_response_data_value(ud, resp);
  exprtk_map_set(&result, "data", data);
  exprtk_value_destroy(&data);
  if (resp && resp->error && resp->error[0]) {
    error = net_make_string_value(ud->env, resp->error, strlen(resp->error));
    exprtk_map_set(&result, "error", error);
    exprtk_value_destroy(&error);
  } else {
    exprtk_map_set(&result, "error", net_null_value());
  }

  return result;
}

/* == Script functions: HTTP ============================================== */

static int net_build_request_headers(http_ud_t *ud, const exprtk_value_t *headers_map,
                                     const char ***items_out, int *count_out) {
  exprtk_map_iter_t it;
  const char *key = NULL;
  exprtk_value_t value;
  const char **items;
  size_t count = 0;
  size_t idx = 0;

  *items_out = NULL;
  *count_out = 0;
  if (!headers_map || headers_map->type != EXPRTK_VAL_MAP) return 1;

  it = exprtk_map_iter_begin(headers_map);
  while (exprtk_map_iter_next(&it, &key, &value)) {
    count++;
  }
  if (count == 0) return 1;

  items = mem_alloc(ud->scratch, count * sizeof(char *));
  if (!items) return 0;

  it = exprtk_map_iter_begin(headers_map);
  while (exprtk_map_iter_next(&it, &key, &value)) {
    char *line;
    size_t key_len;
    size_t value_len;

    if (!key || value.type != EXPRTK_VAL_STRING) continue;

    key_len = strlen(key);
    value_len = value.data.string.len;
    line = mem_alloc(ud->scratch, key_len + 2 + value_len + 1);
    if (!line) return 0;

    memcpy(line, key, key_len);
    line[key_len] = ':';
    line[key_len + 1] = ' ';
    memcpy(line + key_len + 2, value.data.string.data, value_len);
    line[key_len + 2 + value_len] = '\0';
    items[idx++] = line;
  }

  *items_out = items;
  *count_out = (int)idx;
  return 1;
}

typedef struct net_http_request_config_s {
  turbo_http_options_t facade;
  const char **headers;
  int header_count;
  const char *basic_user;
  const char *basic_pass;
  const char *bearer_token;
} net_http_request_config_t;

#define NET_MAX_EXACT_INTEGER 9007199254740991.0

static int net_positive_i64_value(const exprtk_value_t *value, int64_t *out) {
  if (!value || !out) return 0;
  if (value->type == EXPRTK_VAL_INTEGER && value->data.integer > 0) {
    *out = value->data.integer;
    return 1;
  }
  if (value->type == EXPRTK_VAL_NUMBER && isfinite(value->data.number) &&
      value->data.number >= 1.0 && value->data.number <= NET_MAX_EXACT_INTEGER &&
      floor(value->data.number) == value->data.number) {
    *out = (int64_t)value->data.number;
    return 1;
  }
  return 0;
}

static int net_http_transport_value(const exprtk_value_t *value,
                                    turbo_http_transport_t *out) {
  const char *text;
  size_t len;

  if (!value || !out || value->type != EXPRTK_VAL_STRING) return 0;
  text = value->data.string.data;
  len = value->data.string.len;
  if (len == 4 && tstr_ncasecmp(text, "auto", len) == 0) {
    *out = TURBO_HTTP_TRANSPORT_AUTO;
    return 1;
  }
  if (len == 2 && tstr_ncasecmp(text, "h1", len) == 0) {
    *out = TURBO_HTTP_TRANSPORT_H1;
    return 1;
  }
  if (len == 2 && tstr_ncasecmp(text, "h2", len) == 0) {
    *out = TURBO_HTTP_TRANSPORT_H2;
    return 1;
  }
  return 0;
}

static int net_parse_request_options(http_ud_t *ud, const exprtk_value_t *options,
                                     net_http_request_config_t *config) {
  exprtk_value_t value;
  int bool_value;

  if (!ud || !config) return 0;
  memset(config, 0, sizeof(*config));
  if (turbo_http_options_init(&config->facade, sizeof(config->facade)) != TURBO_OK)
    return 0;
  if (!options) return 1;
  if (options->type != EXPRTK_VAL_MAP) return 0;

  value = exprtk_map_get(options, "timeout");
  if (value.type != EXPRTK_VAL_NULL &&
      !net_positive_i64_value(&value, &config->facade.timeout_ms)) {
    return 0;
  }

  value = exprtk_map_get(options, "follow_redirects");
  if (net_truthy_value(&value, &bool_value)) {
    config->facade.follow_redirects = bool_value;
  }

  value = exprtk_map_get(options, "transport");
  if (value.type != EXPRTK_VAL_NULL &&
      !net_http_transport_value(&value, &config->facade.transport)) {
    return 0;
  }

  value = exprtk_map_get(options, "headers");
  if (value.type == EXPRTK_VAL_MAP) {
    if (!net_build_request_headers(ud, &value, &config->headers,
                                   &config->header_count)) return 0;
  }

  value = exprtk_map_get(options, "basic_auth");
  if (value.type == EXPRTK_VAL_MAP) {
    exprtk_value_t user = exprtk_map_get(&value, "user");
    exprtk_value_t pass = exprtk_map_get(&value, "pass");
    if (user.type == EXPRTK_VAL_STRING && pass.type == EXPRTK_VAL_STRING) {
      config->basic_user = net_arena_cstr(ud->scratch, user.data.string);
      config->basic_pass = net_arena_cstr(ud->scratch, pass.data.string);
      if (!config->basic_user || !config->basic_pass) return 0;
    }
  }

  value = exprtk_map_get(options, "bearer_token");
  if (value.type == EXPRTK_VAL_STRING) {
    config->bearer_token = net_arena_cstr(ud->scratch, value.data.string);
    if (!config->bearer_token) return 0;
  }

  return 1;
}

static int net_configure_http_client(turbo_http_t *client,
                                     const net_http_request_config_t *config) {
  if (!client || !config) return 0;
  if (config->basic_user &&
      turbo_http_set_basic_auth(client, config->basic_user, config->basic_pass) != TURBO_OK)
    return 0;
  if (config->bearer_token &&
      turbo_http_set_bearer_token(client, config->bearer_token) != TURBO_OK)
    return 0;
  return 1;
}

static exprtk_value_t net_http_request(http_ud_t *ud, http_method_t method, tstr_v url_sv,
                                       const tstr_v *body_sv, const exprtk_value_t *options,
                                       int structured_response, exprtk_env_t *env) {
  turbo_http_t *client = NULL;
  http_response_t *resp;
  net_http_request_config_t config;
  coro_context_t *coro_ctx;
  char *url;
  const char *body = NULL;
  size_t body_len = 0;
  exprtk_value_t ret = NET_ZERO;
  http_request_control_t control = HTTP_REQUEST_CONTROL_DEFAULT;
  http_ud_t call_ud;
  int create_result;

  if (!ud || !env) return NET_ZERO;
  call_ud = *ud;
  call_ud.env = env;
  ud = &call_ud;

  if (!net_parse_request_options(ud, options, &config)) {
    return structured_response ? net_error_response_value(ud, "invalid HTTP request options")
                               : NET_ZERO;
  }

  url = net_arena_cstr(ud->scratch, url_sv);
  if (!url) {
    return structured_response ? net_error_response_value(ud, "URL allocation failed")
                               : NET_ZERO;
  }

  if (body_sv) {
    body = body_sv->data;
    body_len = body_sv->len;
  }

  coro_ctx = coro_context_current();
  create_result = coro_ctx ? turbo_http_create(coro_ctx, &config.facade, &client)
                           : turbo_http_create_sync(&config.facade, &client);
  if (create_result != TURBO_OK || !client) {
    return structured_response ? net_error_response_value(ud, "HTTP facade create failed")
                               : NET_ZERO;
  }
  if (!net_configure_http_client(client, &config)) {
    turbo_http_destroy(client);
    return structured_response ? net_error_response_value(ud, "HTTP authentication setup failed")
                               : NET_ZERO;
  }

  control.cancel_token = turbo_script_current_task_cancel_token();
  resp = coro_ctx ? turbo_http_request_ex(client, method, url, config.headers,
                                          config.header_count, body, body_len, &control)
                  : turbo_http_request_sync(client, method, url, config.headers,
                                            config.header_count, body, body_len);
  if (resp) {
    ret = structured_response ? net_response_value(ud, resp) : copy_response_body(resp, ud->env);
    http_response_free(resp);
  } else if (structured_response) {
    ret = net_error_response_value(ud, "HTTP request failed");
  }

  turbo_http_destroy(client);
  return ret;
}

static exprtk_value_t fn_http_get(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;

  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    return net_http_request(ud, HTTP_GET, args[0].data.string, NULL, NULL, 0, env);
  }
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_MAP) {
    return net_http_request(ud, HTTP_GET, args[0].data.string, NULL, &args[1], 1, env);
  }

  return NET_ZERO;
}

static exprtk_value_t fn_http_post(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;

  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    return net_http_request(ud, HTTP_POST, args[0].data.string, &args[1].data.string, NULL, 0,
                            env);
  }
  if (argc == 3 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING &&
      args[2].type == EXPRTK_VAL_MAP) {
    return net_http_request(ud, HTTP_POST, args[0].data.string, &args[1].data.string, &args[2], 1,
                            env);
  }

  return NET_ZERO;
}

/* == Script functions: WebSocket ========================================= */

static exprtk_value_t fn_ws_connect(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  const coro_cancel_token_t *owner;
  coro_cancel_registration_t *cancel_registration = NULL;
  coro_socket_t *client;
  char *url;
  const char *host = NULL;
  const char *path = NULL;
  size_t host_len = 0;
  int port = 0;
  int is_tls = 0;
  char *host_buf = NULL;
  int r;

  if (!ud || !ud->ctx || argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    return NET_ZERO;
  }
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING) {
    return NET_ZERO;
  }

  owner = turbo_script_current_task_cancel_token();
  client = net_ctx_recreate_ws_client(ud->ctx, owner);
  if (!client) {
    return NET_ZERO;
  }

  url = net_arena_cstr(ud->scratch, args[0].data.string);
  if (!url) {
    net_set_error(ud->ctx, "url alloc failed");
    goto fail;
  }

  if (net_parse_ws_url(url, &host, &host_len, &port, &path, &is_tls) != 0) {
    net_set_error(ud->ctx, "invalid ws url");
    goto fail;
  }

  host_buf = mem_alloc(ud->scratch, host_len + 1);
  if (!host_buf) {
    net_set_error(ud->ctx, "host alloc failed");
    goto fail;
  }
  memcpy(host_buf, host, host_len);
  host_buf[host_len] = '\0';

  coro_socket_set_timeout(client, 10000);
  if (owner) {
    r = coro_cancel_register(owner, net_ws_cancel_wait, client, &cancel_registration);
    if (r != TURBO_OK) {
      net_set_error(ud->ctx, r == TURBO_ECANCELED ? "ws connect cancelled"
                                                  : "ws connect cancellation setup failed");
      goto fail;
    }
  }
  r = coro_socket_connect_ws(client, host_buf, port, path, is_tls);
  if (cancel_registration) {
    (void)coro_cancel_unregister(cancel_registration);
    cancel_registration = NULL;
  }
  if (r != 0) {
    net_set_error(ud->ctx, r == TURBO_ECANCELED ? "ws connect cancelled"
                                                : "ws connect failed");
    goto fail;
  }

  net_set_error(ud->ctx, "");
  return NET_ONE;

fail:
  if (cancel_registration) (void)coro_cancel_unregister(cancel_registration);
  {
    coro_socket_t **slot = net_ctx_ws_slot(ud->ctx, owner, 0);
    if (slot) net_ctx_clear_ws_slot(ud->ctx, owner, slot);
  }
  return NET_ZERO;
}

static exprtk_value_t fn_ws_send(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  const coro_cancel_token_t *owner;
  coro_cancel_registration_t *cancel_registration = NULL;
  coro_socket_t *client;
  tstr_v payload;
  int r;

  if (!ud || !ud->ctx || argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    return NET_ZERO;
  }
  owner = turbo_script_current_task_cancel_token();
  client = net_ctx_current_ws_client(ud->ctx, owner);
  if (!client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }

  payload = args[0].data.string;
  if (owner) {
    r = coro_cancel_register(owner, net_ws_cancel_wait, client, &cancel_registration);
    if (r != TURBO_OK) {
      net_set_error(ud->ctx, r == TURBO_ECANCELED ? "ws send cancelled"
                                                  : "ws send cancellation setup failed");
      return NET_ZERO;
    }
  }
  r = coro_socket_send(client, payload.data, payload.len);
  if (cancel_registration) (void)coro_cancel_unregister(cancel_registration);
  if (r < 0) {
    net_set_error(ud->ctx, r == TURBO_ECANCELED ? "ws send cancelled"
                                                : "ws send failed");
    return NET_ZERO;
  }

  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static void net_ws_release_callback_env(exprtk_env_t *callback_env) {
  if (callback_env) exprtk_env_release(callback_env);
}

static exprtk_value_t fn_ws_consume(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  const coro_cancel_token_t *owner;
  const coro_cancel_token_t *cancel_token;
  coro_cancel_registration_t *cancel_registration = NULL;
  coro_socket_t *client;
  int timeout_ms = 5000;
  char *resp = NULL;
  size_t len = 0;
  int r;
  exprtk_value_t message;
  exprtk_value_t result;
  exprtk_env_t *call_env;
  exprtk_env_t *callback_env;

  if (!ud || !ud->ctx || argc != 2 || args[0].type != EXPRTK_VAL_NUMBER ||
      args[1].type != EXPRTK_VAL_FUNCTION)
    return NET_ZERO;
  call_env = env;
  if (!call_env) return NET_ZERO;
  callback_env = args[1].data.function.closure_env;
  if (callback_env) exprtk_env_retain(callback_env);
  owner = turbo_script_current_task_cancel_token();
  client = net_ctx_current_ws_client(ud->ctx, owner);
  if (!client) {
    net_set_error(ud->ctx, "ws client not connected");
    net_ws_release_callback_env(callback_env);
    return NET_ZERO;
  }

  timeout_ms = (int)args[0].data.number;

  if (timeout_ms < 0) {
    net_set_error(ud->ctx, "ws recv timeout must be non-negative");
    net_ws_release_callback_env(callback_env);
    return NET_ZERO;
  }
  coro_socket_set_timeout(client, (uint64_t)timeout_ms);

  cancel_token = owner;
  if (cancel_token) {
    r = coro_cancel_register(cancel_token, net_ws_cancel_wait, client,
                             &cancel_registration);
    if (r != TURBO_OK) {
      net_set_error(ud->ctx, r == TURBO_ECANCELED ? "ws recv cancelled"
                                                  : "ws recv cancellation setup failed");
      net_ws_release_callback_env(callback_env);
      return NET_ZERO;
    }
  }

  r = coro_socket_recv(client, &resp, &len);
  if (cancel_registration) (void)coro_cancel_unregister(cancel_registration);
  if (r != 0 || !resp) {
    net_set_error(ud->ctx, r == TURBO_ECANCELED ? "ws recv cancelled"
                                                : "ws recv failed or timeout");
    if (resp) coro_socket_free_recv(resp);
    net_ws_release_callback_env(callback_env);
    return NET_ZERO;
  }

  if (len > call_env->max_external_value_bytes) {
    coro_socket_free_recv(resp);
    net_set_error(ud->ctx, "ws frame exceeds the configured external-value quota");
    call_env->aborted = 1;
    snprintf(call_env->error_msg, sizeof(call_env->error_msg),
             "ws.consume: frame size %zu exceeds quota %zu", len,
             call_env->max_external_value_bytes);
    net_ws_release_callback_env(callback_env);
    return NET_ZERO;
  }

  /* The transport buffer is borrowed only for this callback. Script function
   * argument binding copies it into the callback-local environment; only a
   * value explicitly returned by the callback may escape into the task env. */
  message = exprtk_val_str(tstr_v_from_buf(resp, len));
  result = exprtk_call_function_value(args[1], 1, &message, call_env);
  coro_socket_free_recv(resp);
  net_ws_release_callback_env(callback_env);
  net_set_error(ud->ctx, "");
  return result;
}

static exprtk_value_t fn_ws_close(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  const coro_cancel_token_t *owner;
  coro_socket_t **slot;
  (void)args;
  if (!ud || !ud->ctx || argc != 0) return NET_ZERO;

  owner = turbo_script_current_task_cancel_token();
  slot = net_ctx_ws_slot(ud->ctx, owner, 0);
  if (slot) net_ctx_clear_ws_slot(ud->ctx, owner, slot);
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

/* == Loader =============================================================== */

void net_load(void *p, void *e, void *s) {
  net_ctx_t *ctx = (net_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  mem_pool_t *scratch = (mem_pool_t *)s;
  http_ud_t *ud;

  if (!ctx || !env) return;

  ud = mem_alloc(&env->arena, sizeof(*ud));
  if (!ud) return;

  ud->ctx = ctx;
  ud->env = env;
  ud->scratch = scratch;

  exprtk_env_register_func(env, "http.get", fn_http_get, ud);
  exprtk_env_register_func(env, "http.post", fn_http_post, ud);
  exprtk_env_register_func(env, "ws.connect", fn_ws_connect, ud);
  exprtk_env_register_func(env, "ws.send", fn_ws_send, ud);
  exprtk_env_register_func(env, "ws.consume", fn_ws_consume, ud);
  exprtk_env_register_func(env, "ws.close", fn_ws_close, ud);
}
