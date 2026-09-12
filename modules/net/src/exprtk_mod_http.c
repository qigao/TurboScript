/**
 * @file exprtk_mod_http.c
 * @brief HTTP and WebSocket module: http.* and ws.* for TurboScript.
 */
#include "net_ctx.h"
#include "turbo_script.h"
#include "exprtk_module.h"
#include <turbo_parser_json.h>
#include <tstr.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define NET_ONE ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0})

/* == Lifecycle ============================================================ */

static native_io_backend_kind net_io_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config net_client_network_config(size_t connection_capacity) {
  const cnet_client_config config = {
      .backend = net_io_backend(),
      .connection_capacity = connection_capacity,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 1024u * 1024u,
      .receive_buffer_bytes = 64u * 1024u,
      .connect_timeout_ms = 10000u,
      .read_timeout_ms = 30000u,
      .write_timeout_ms = 30000u};
  return config;
}

static chttp_client_config net_http_client_config(void) {
  const chttp_client_config config = {
      .network = net_client_network_config(2u),
      .request_capacity = 1u,
      .max_start_line_bytes = 4096u,
      .max_header_count = 64u,
      .max_header_bytes = 32u * 1024u,
      .max_request_body_bytes = 1024u * 1024u,
      .max_response_body_bytes = 1024u * 1024u,
      .max_informational_responses = 4u};
  return config;
}

static chttp_websocket_client_config net_ws_client_config(void) {
  const chttp_websocket_client_config config = {
      .size = sizeof(config),
      .network = net_client_network_config(1u),
      .max_frame_bytes = 64u * 1024u,
      .max_message_bytes = 64u * 1024u,
      .max_buffered_input_bytes = 64u * 1024u,
      .max_handshake_header_bytes = 16u * 1024u,
      .event_capacity = 16u};
  return config;
}

void *net_ctx_create(void) {
  return calloc(1, sizeof(net_ctx_t));
}

chttp_client *net_ctx_ensure_client(net_ctx_t *ctx) {
  chttp_client_config config;
  chttp_client *client;

  if (!ctx) return NULL;
  if (ctx->client) return ctx->client;
  client = calloc(1u, sizeof(*client));
  if (!client) return NULL;
  config = net_http_client_config();
  if (chttp_client_init(client, &config) != SALTS_OK) {
    free(client);
    return NULL;
  }
  ctx->client = client;
  return client;
}

void net_ctx_destroy(void *p) {
  net_ctx_t *ctx = (net_ctx_t *)p;
  size_t i;
  if (!ctx) return;
  if (ctx->client) {
    (void)chttp_client_destroy(ctx->client, 10000u);
    free(ctx->client);
  }
  if (ctx->ws_client) {
    (void)chttp_websocket_client_destroy(ctx->ws_client, 10000u);
    free(ctx->ws_client);
  }
  for (i = 0; i < NET_WS_TASK_CONNECTION_CAPACITY; ++i) {
    if (ctx->ws_task_connections[i].client) {
      (void)chttp_websocket_client_destroy(ctx->ws_task_connections[i].client, 10000u);
      free(ctx->ws_task_connections[i].client);
    }
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
  exprtk_value_t borrowed = exprtk_val_str(vstr_from_buf(data ? data : "", len));
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

static exprtk_value_t copy_response_body(const chttp_response *resp, exprtk_env_t *env) {
  if (resp && resp->status_code >= 200 && resp->status_code < 300 && resp->body) {
    return net_make_string_value(env, (const char *)resp->body, resp->body_size);
  }
  return NET_ZERO;
}

static void net_set_error(net_ctx_t *ctx, const char *msg) {
  if (!ctx) return;
  snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg ? msg : "");
}

static chttp_websocket_client **net_ctx_ws_slot(net_ctx_t *ctx, const void *owner, int create) {
  size_t i;
  net_ws_task_connection_t *free_slot = NULL;

  if (!ctx) return NULL;
  if (!owner) return &ctx->ws_client;

  for (i = 0; i < NET_WS_TASK_CONNECTION_CAPACITY; ++i) {
    net_ws_task_connection_t *entry = &ctx->ws_task_connections[i];
    if (entry->client && entry->owner == owner) return &entry->client;
    if (!entry->client && !free_slot) free_slot = entry;
  }
  if (!create || !free_slot) return NULL;

  free_slot->owner = owner;
  ctx->ws_task_connection_count++;
  return &free_slot->client;
}

static void net_ctx_clear_ws_slot(net_ctx_t *ctx, const void *owner,
                                  chttp_websocket_client **slot) {
  size_t i;

  if (!ctx || !slot) return;
  if (*slot) {
    (void)chttp_websocket_client_destroy(*slot, 10000u);
    free(*slot);
  }
  *slot = NULL;
  if (!owner) return;

  for (i = 0; i < NET_WS_TASK_CONNECTION_CAPACITY; ++i) {
    net_ws_task_connection_t *entry = &ctx->ws_task_connections[i];
    if (&entry->client != slot) continue;
    entry->owner = NULL;
    if (ctx->ws_task_connection_count > 0) ctx->ws_task_connection_count--;
    return;
  }
}

static chttp_websocket_client *net_ctx_recreate_ws_client(net_ctx_t *ctx, const void *owner) {
  chttp_websocket_client **slot;
  chttp_websocket_client_config config;

  if (!ctx) return NULL;
  slot = net_ctx_ws_slot(ctx, owner, 1);
  if (!slot) {
    net_set_error(ctx, "ws task connection capacity exhausted");
    return NULL;
  }
  if (*slot) net_ctx_clear_ws_slot(ctx, owner, slot);
  slot = net_ctx_ws_slot(ctx, owner, 1);
  if (!slot) return NULL;

  *slot = calloc(1u, sizeof(**slot));
  if (*slot) {
    config = net_ws_client_config();
    if (chttp_websocket_client_init(*slot, &config) != SALTS_OK) {
      free(*slot);
      *slot = NULL;
    }
  }
  if (!*slot) net_ctx_clear_ws_slot(ctx, owner, slot);
  return *slot;
}

static chttp_websocket_client *net_ctx_current_ws_client(net_ctx_t *ctx, const void *owner) {
  chttp_websocket_client **slot = net_ctx_ws_slot(ctx, owner, 0);
  return slot ? *slot : NULL;
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

static exprtk_value_t net_response_headers_map(http_ud_t *ud, const chttp_response *resp) {
  exprtk_value_t headers = exprtk_val_map();
  size_t i;

  if (!resp || !resp->headers || resp->header_count == 0u) return headers;
  for (i = 0; i < resp->header_count; ++i) {
    const chttp_header *header = &resp->headers[i];
    exprtk_value_t header_value;
    if (!header->name || !header->value) continue;
    header_value = net_make_string_value(ud->env, header->value, strlen(header->value));
    exprtk_map_set(&headers, header->name, header_value);
    exprtk_value_destroy(&header_value);
  }
  return headers;
}

static int net_response_is_json(const chttp_response *resp) {
  const char *content_type;
  if (!resp) return 0;
  content_type = chttp_response_header(resp, "content-type");
  return content_type && strstr(content_type, "json") != NULL;
}

static exprtk_value_t net_response_data_value(http_ud_t *ud, const chttp_response *resp) {
  exprtk_value_t value = net_null_value();
  json_value_t *json = NULL;

  if (!resp || !resp->body || !net_response_is_json(resp)) return value;

  if (turbo_parse_json((const uint8_t *)resp->body, resp->body_size, &json) != 0 || !json)
    return value;

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

static exprtk_value_t net_response_value(http_ud_t *ud, const chttp_response *resp) {
  exprtk_value_t result = exprtk_val_map();
  exprtk_value_t body;
  exprtk_value_t error;
  exprtk_value_t headers;
  exprtk_value_t data;

  exprtk_map_set(&result, "status", exprtk_val_num(resp ? (double)resp->status_code : 0.0));
  body = net_make_string_value(ud->env, resp && resp->body ? (const char *)resp->body : "",
                               resp && resp->body ? resp->body_size : 0u);
  exprtk_map_set(&result, "body", body);
  exprtk_value_destroy(&body);
  headers = net_response_headers_map(ud, resp);
  exprtk_map_set(&result, "headers", headers);
  exprtk_value_destroy(&headers);
  data = net_response_data_value(ud, resp);
  exprtk_map_set(&result, "data", data);
  exprtk_value_destroy(&data);
  (void)error;
  exprtk_map_set(&result, "error", net_null_value());

  return result;
}

/* == Script functions: HTTP ============================================== */

static int net_build_request_headers(http_ud_t *ud, const exprtk_value_t *headers_map,
                                     chttp_header **items_out, size_t *count_out) {
  exprtk_map_iter_t it;
  const char *key = NULL;
  exprtk_value_t value;
  chttp_header *items;
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

  items = mem_alloc(ud->scratch, count * sizeof(*items));
  if (!items) return 0;

  it = exprtk_map_iter_begin(headers_map);
  while (exprtk_map_iter_next(&it, &key, &value)) {
    char *item_value;
    if (!key || value.type != EXPRTK_VAL_STRING || key[0] == '\0') return 0;
    item_value = net_arena_cstr(ud->scratch, value.data.string);
    if (!item_value) return 0;
    items[idx++] = (chttp_header){.name = key, .value = item_value};
  }

  *items_out = items;
  *count_out = idx;
  return 1;
}

typedef struct net_http_request_config_s {
  chttp_protocol protocol;
  uint32_t timeout_ms;
  chttp_header *headers;
  size_t header_count;
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

static int net_http_transport_value(const exprtk_value_t *value, chttp_protocol *out) {
  const char *text;
  size_t len;

  if (!value || !out || value->type != EXPRTK_VAL_STRING) return 0;
  text = value->data.string.data;
  len = value->data.string.len;
  if (len == 2 && tstr_ncasecmp(text, "h1", len) == 0) {
    *out = CHTTP_HTTP_1_1;
    return 1;
  }
  if (len == 2 && tstr_ncasecmp(text, "h2", len) == 0) {
    *out = CHTTP_HTTP_2;
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
  config->protocol = CHTTP_HTTP_1_1;
  if (!options) return 1;
  if (options->type != EXPRTK_VAL_MAP) return 0;

  if (exprtk_map_has(options, "timeout")) {
    value = exprtk_map_get(options, "timeout");
    int64_t timeout_ms;
    if (!net_positive_i64_value(&value, &timeout_ms) || timeout_ms > UINT32_MAX) return 0;
    config->timeout_ms = (uint32_t)timeout_ms;
  }

  if (exprtk_map_has(options, "follow_redirects")) {
    value = exprtk_map_get(options, "follow_redirects");
    if (!net_truthy_value(&value, &bool_value) || bool_value) return 0;
  }

  if (exprtk_map_has(options, "transport")) {
    value = exprtk_map_get(options, "transport");
    if (!net_http_transport_value(&value, &config->protocol)) return 0;
  }

  if (exprtk_map_has(options, "headers")) {
    value = exprtk_map_get(options, "headers");
    if (value.type == EXPRTK_VAL_MAP &&
        !net_build_request_headers(ud, &value, &config->headers,
                                   &config->header_count)) {
      return 0;
    }
  }

  if (exprtk_map_has(options, "basic_auth")) {
    value = exprtk_map_get(options, "basic_auth");
    if (value.type == EXPRTK_VAL_MAP) {
      exprtk_value_t user = exprtk_map_get(&value, "user");
      exprtk_value_t pass = exprtk_map_get(&value, "pass");
      if (user.type != EXPRTK_VAL_STRING || pass.type != EXPRTK_VAL_STRING) return 0;
      config->basic_user = net_arena_cstr(ud->scratch, user.data.string);
      config->basic_pass = net_arena_cstr(ud->scratch, pass.data.string);
      if (!config->basic_user || !config->basic_pass) return 0;
    } else return 0;
  }

  if (exprtk_map_has(options, "bearer_token")) {
    value = exprtk_map_get(options, "bearer_token");
    if (value.type != EXPRTK_VAL_STRING) return 0;
    config->bearer_token = net_arena_cstr(ud->scratch, value.data.string);
    if (!config->bearer_token) return 0;
  }

  return !(config->basic_user && config->bearer_token);
}

typedef struct net_http_uri_s {
  char *connection_uri;
  char *authority;
  char *target;
} net_http_uri_t;

static int net_http_uri_parse(mem_pool_t *scratch, const char *url, net_http_uri_t *out) {
  const char *authority;
  const char *end;
  const char *target;
  const char *port_separator = NULL;
  size_t authority_len;
  size_t scheme_len;
  unsigned int default_port;
  int has_port = 0;
  const char *transport;

  if (!scratch || !url || !out) return 0;
  memset(out, 0, sizeof(*out));
  if (strchr(url, '#')) return 0;
  authority = strstr(url, "://");
  if (!authority) return 0;
  scheme_len = (size_t)(authority - url);
  if (scheme_len == 4u && strncmp(url, "http", scheme_len) == 0) {
    transport = "tcp";
    default_port = 80u;
  } else if (scheme_len == 5u && strncmp(url, "https", scheme_len) == 0) {
    transport = "tls";
    default_port = 443u;
  } else return 0;
  authority += 3;
  end = authority;
  while (*end && *end != '/' && *end != '?') ++end;
  authority_len = (size_t)(end - authority);
  if (authority_len == 0u || memchr(authority, '@', authority_len)) return 0;
  if (authority[0] == '[') {
    const char *bracket = memchr(authority, ']', authority_len);
    if (!bracket || bracket == authority + 1) return 0;
    if (bracket + 1 < end) {
      if (bracket[1] != ':') return 0;
      port_separator = bracket + 1;
      has_port = 1;
    }
  } else {
    const char *colon = memchr(authority, ':', authority_len);
    if (colon) {
      if (memchr(colon + 1, ':', (size_t)(end - (colon + 1)))) return 0;
      port_separator = colon;
      has_port = 1;
    }
  }
  if (has_port) {
    const char *port = port_separator + 1;
    unsigned long value = 0u;
    if (port == end) return 0;
    while (port < end) {
      if (*port < '0' || *port > '9' || value > 6553u) return 0;
      value = value * 10u + (unsigned long)(*port++ - '0');
    }
    if (value == 0u || value > 65535u) return 0;
  }
  out->authority = mem_alloc(scratch, authority_len + 1u);
  if (!out->authority) return 0;
  memcpy(out->authority, authority, authority_len);
  out->authority[authority_len] = '\0';
  if (has_port) {
    out->connection_uri = mem_alloc(scratch, strlen(transport) + 3u + authority_len + 1u);
    if (!out->connection_uri) return 0;
    snprintf(out->connection_uri, strlen(transport) + 3u + authority_len + 1u, "%s://%s",
             transport, out->authority);
  } else {
    size_t needed = strlen(transport) + 3u + authority_len + 1u + 5u + 1u;
    out->connection_uri = mem_alloc(scratch, needed);
    if (!out->connection_uri) return 0;
    snprintf(out->connection_uri, needed, "%s://%s:%u", transport, out->authority, default_port);
  }
  target = *end ? end : "/";
  if (*target == '?') {
    size_t target_len = strlen(target);
    out->target = mem_alloc(scratch, target_len + 2u);
    if (!out->target) return 0;
    out->target[0] = '/';
    memcpy(out->target + 1, target, target_len + 1u);
  } else {
    out->target = (char *)target;
  }
  return 1;
}

static int net_append_authorization(http_ud_t *ud, net_http_request_config_t *config) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  chttp_header *headers;
  char *authorization;
  size_t input_len;
  size_t encoded_len;
  size_t index = 0u;
  size_t out = 0u;
  size_t i;

  if (!config->basic_user && !config->bearer_token) return 1;
  for (i = 0; i < config->header_count; ++i) {
    if (strlen(config->headers[i].name) == 13u &&
        tstr_ncasecmp(config->headers[i].name, "authorization", 13u) == 0)
      return 0;
  }
  headers = mem_alloc(ud->scratch, (config->header_count + 1u) * sizeof(*headers));
  if (!headers) return 0;
  if (config->header_count) memcpy(headers, config->headers, config->header_count * sizeof(*headers));
  if (config->bearer_token) {
    size_t token_len = strlen(config->bearer_token);
    authorization = mem_alloc(ud->scratch, sizeof("Bearer ") - 1u + token_len + 1u);
    if (!authorization) return 0;
    memcpy(authorization, "Bearer ", sizeof("Bearer ") - 1u);
    memcpy(authorization + sizeof("Bearer ") - 1u, config->bearer_token, token_len + 1u);
  } else {
    char *credentials;
    size_t user_len = strlen(config->basic_user);
    size_t pass_len = strlen(config->basic_pass);
    input_len = strlen(config->basic_user) + 1u + strlen(config->basic_pass);
    if (input_len > (SIZE_MAX - 2u) / 3u) return 0;
    encoded_len = ((input_len + 2u) / 3u) * 4u;
    authorization = mem_alloc(ud->scratch, sizeof("Basic ") - 1u + encoded_len + 1u);
    credentials = mem_alloc(ud->scratch, input_len);
    if (!authorization || !credentials) return 0;
    memcpy(credentials, config->basic_user, user_len);
    credentials[user_len] = ':';
    memcpy(credentials + user_len + 1u, config->basic_pass, pass_len);
    memcpy(authorization, "Basic ", sizeof("Basic ") - 1u);
    while (index < input_len) {
      unsigned int block = 0u;
      size_t left = input_len - index;
      unsigned char first = (unsigned char)credentials[index];
      unsigned char second = left > 1u ? (unsigned char)credentials[index + 1u] : 0u;
      unsigned char third = left > 2u ? (unsigned char)credentials[index + 2u] : 0u;
      block = ((unsigned int)first << 16u) | ((unsigned int)second << 8u) | (unsigned int)third;
      authorization[sizeof("Basic ") - 1u + out++] = alphabet[(block >> 18u) & 63u];
      authorization[sizeof("Basic ") - 1u + out++] = alphabet[(block >> 12u) & 63u];
      authorization[sizeof("Basic ") - 1u + out++] = left > 1u ? alphabet[(block >> 6u) & 63u] : '=';
      authorization[sizeof("Basic ") - 1u + out++] = left > 2u ? alphabet[block & 63u] : '=';
      index += left >= 3u ? 3u : left;
    }
    authorization[sizeof("Basic ") - 1u + out] = '\0';
  }
  headers[config->header_count++] = (chttp_header){.name = "Authorization", .value = authorization};
  config->headers = headers;
  return 1;
}

static int net_http_call(chttp_client *client, chttp_method method, const chttp_options *options,
                         chttp_response *response, chttp_error *error) {
  switch (method) {
    case CHTTP_METHOD_GET: return chttp_get(client, options, response, error);
    case CHTTP_METHOD_POST: return chttp_post(client, options, response, error);
    default: return SALTS_EINVAL;
  }
}

static exprtk_value_t net_http_request(http_ud_t *ud, chttp_method method, vstr url_sv,
                                       const vstr *body_sv, const exprtk_value_t *options,
                                       int structured_response, exprtk_env_t *env) {
  chttp_client *client;
  chttp_response response = {0};
  chttp_error error = {0};
  chttp_options request = {0};
  net_http_request_config_t config;
  net_http_uri_t uri;
  char *url;
  const char *body = NULL;
  size_t body_len = 0;
  exprtk_value_t ret = NET_ZERO;
  http_ud_t call_ud;
  int status;

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
  if (!net_http_uri_parse(ud->scratch, url, &uri)) {
    return structured_response ? net_error_response_value(ud, "invalid HTTP URL") : NET_ZERO;
  }

  if (body_sv) {
    body = body_sv->data;
    body_len = body_sv->len;
  }

  if (!net_append_authorization(ud, &config)) {
    return structured_response ? net_error_response_value(ud, "invalid HTTP authorization")
                               : NET_ZERO;
  }
  client = net_ctx_ensure_client(ud->ctx);
  if (!client) {
    return structured_response ? net_error_response_value(ud, "CHTTP client initialization failed")
                               : NET_ZERO;
  }
  request = (chttp_options){.connection_uri = uri.connection_uri,
                            .authority = uri.authority,
                            .target = uri.target,
                            .headers = config.headers,
                            .header_count = config.header_count,
                            .body = body,
                            .body_size = body_len,
                            .timeout_ms = config.timeout_ms,
                            .protocol = config.protocol};
  status = net_http_call(client, method, &request, &response, &error);
  if (status == SALTS_OK) {
    ret = structured_response ? net_response_value(ud, &response) : copy_response_body(&response, ud->env);
    chttp_response_destroy(&response);
  } else if (structured_response) {
    ret = net_error_response_value(ud, error.stage ? error.stage : "CHTTP request failed");
  }
  return ret;
}

static exprtk_value_t fn_http_get(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;

  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    return net_http_request(ud, CHTTP_METHOD_GET, args[0].data.string, NULL, NULL, 0, env);
  }
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_MAP) {
    return net_http_request(ud, CHTTP_METHOD_GET, args[0].data.string, NULL, &args[1], 1, env);
  }

  return NET_ZERO;
}

static exprtk_value_t fn_http_post(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;

  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    return net_http_request(ud, CHTTP_METHOD_POST, args[0].data.string, &args[1].data.string, NULL, 0,
                            env);
  }
  if (argc == 3 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING &&
      args[2].type == EXPRTK_VAL_MAP) {
    return net_http_request(ud, CHTTP_METHOD_POST, args[0].data.string, &args[1].data.string, &args[2], 1,
                            env);
  }

  return NET_ZERO;
}

/* == Script functions: WebSocket ========================================= */

static const void *net_ws_owner(void) {
  return NULL;
}

static exprtk_value_t fn_ws_connect(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  const void *owner;
  chttp_websocket_client *client;
  chttp_websocket_connect_options options = {0};
  char *url;
  unsigned int status_code = 0u;
  int status;
  (void)env;

  if (!ud || !ud->ctx || argc < 1u || argc > 2u || args[0].type != EXPRTK_VAL_STRING ||
      (argc == 2u && args[1].type != EXPRTK_VAL_STRING))
    return NET_ZERO;
  if (argc == 2u) {
    net_set_error(ud->ctx, "CHTTP subprotocol selection is unavailable in this Salts package");
    return NET_ZERO;
  }
  url = net_arena_cstr(ud->scratch, args[0].data.string);
  if (!url) {
    net_set_error(ud->ctx, "ws URL allocation failed");
    return NET_ZERO;
  }
  owner = net_ws_owner();
  client = net_ctx_recreate_ws_client(ud->ctx, owner);
  if (!client) {
    net_set_error(ud->ctx, "CHTTP WebSocket client initialization failed");
    return NET_ZERO;
  }
  options = (chttp_websocket_connect_options){.size = sizeof(options),
                                               .uri = url,
                                               .timeout_ms = 10000u,
                                               .protocol = CHTTP_HTTP_1_1};
  status = chttp_websocket_client_connect(client, &options, &status_code);
  if (status != SALTS_OK) {
    chttp_websocket_client **slot = net_ctx_ws_slot(ud->ctx, owner, 0);
    net_set_error(ud->ctx, "CHTTP WebSocket connect failed");
    if (slot) net_ctx_clear_ws_slot(ud->ctx, owner, slot);
    return NET_ZERO;
  }
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static exprtk_value_t fn_ws_send(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  chttp_websocket_client *client;
  int status;
  (void)env;
  if (!ud || !ud->ctx || argc != 1u || args[0].type != EXPRTK_VAL_STRING) return NET_ZERO;
  client = net_ctx_current_ws_client(ud->ctx, net_ws_owner());
  if (!client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }
  status = chttp_websocket_client_send_text(client, args[0].data.string.data,
                                             args[0].data.string.len, 10000u);
  if (status != SALTS_OK) {
    net_set_error(ud->ctx, "CHTTP WebSocket send failed");
    return NET_ZERO;
  }
  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static void net_ws_release_callback_env(exprtk_env_t *callback_env) {
  if (callback_env) exprtk_env_release(callback_env);
}

static exprtk_value_t fn_ws_consume(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  chttp_websocket_client *client;
  chttp_websocket_event event = {0};
  exprtk_env_t *callback_env;
  exprtk_value_t message;
  exprtk_value_t result;
  int64_t timeout_ms;
  int status;

  if (!ud || !ud->ctx || !env || argc != 2u || args[0].type != EXPRTK_VAL_NUMBER ||
      args[1].type != EXPRTK_VAL_FUNCTION || !net_positive_i64_value(&args[0], &timeout_ms) ||
      timeout_ms > UINT32_MAX)
    return NET_ZERO;
  callback_env = args[1].data.function.closure_env;
  if (callback_env) exprtk_env_retain(callback_env);
  client = net_ctx_current_ws_client(ud->ctx, net_ws_owner());
  if (!client) {
    net_set_error(ud->ctx, "ws client not connected");
    net_ws_release_callback_env(callback_env);
    return NET_ZERO;
  }
  status = chttp_websocket_client_receive(client, (uint32_t)timeout_ms, &event);
  if (status != SALTS_OK || event.kind != CHTTP_WEBSOCKET_EVENT_MESSAGE) {
    net_set_error(ud->ctx, status == SALTS_OK ? "ws received a non-message event"
                                               : "CHTTP WebSocket receive failed or timed out");
    net_ws_release_callback_env(callback_env);
    return NET_ZERO;
  }
  if (event.size > env->max_external_value_bytes) {
    net_set_error(ud->ctx, "ws frame exceeds the configured external-value quota");
    env->aborted = 1;
    snprintf(env->error_msg, sizeof(env->error_msg),
             "ws.consume: frame size %zu exceeds quota %zu", event.size, env->max_external_value_bytes);
    net_ws_release_callback_env(callback_env);
    return NET_ZERO;
  }
  message = exprtk_val_str(vstr_from_buf((const char *)event.data, event.size));
  result = exprtk_call_function_value(args[1], 1u, &message, env);
  net_ws_release_callback_env(callback_env);
  net_set_error(ud->ctx, "");
  return result;
}

static exprtk_value_t fn_ws_close(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  const void *owner;
  chttp_websocket_client **slot;
  (void)args;
  (void)env;
  if (!ud || !ud->ctx || argc != 0u) return NET_ZERO;
  owner = net_ws_owner();
  slot = net_ctx_ws_slot(ud->ctx, owner, 0);
  if (slot && *slot) {
    (void)chttp_websocket_client_close(*slot, 1000u, NULL, 0u, 10000u);
    net_ctx_clear_ws_slot(ud->ctx, owner, slot);
  }
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
