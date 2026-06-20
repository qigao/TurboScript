/**
 * @file exprtk_mod_http.c
 * @brief HTTP and WebSocket module: http.* and ws.* for TurboScript.
 */
#include "net_ctx.h"
#include "exprtk_module.h"
#include <CoroNet.h>
#include <turbo_parser.h>
#include <turbo_str.h>
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
  if (!ctx) return;
  if (ctx->client) http_client_destroy(ctx->client);
  if (ctx->ws_client) coro_socket_destroy(ctx->ws_client);
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
  char *buf = mem_alloc(&env->arena, len + 1);
  if (!buf) return NET_ZERO;

  if (data && len > 0) {
    memcpy(buf, data, len);
  }
  buf[len] = '\0';
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t copy_response_body(http_response_t *resp, exprtk_env_t *env) {
  if (resp && resp->status_code >= 200 && resp->status_code < 300 && resp->body) {
    char *buf = mem_alloc(&env->arena, resp->body_len + 1);
    if (buf) {
      memcpy(buf, resp->body, resp->body_len);
      buf[resp->body_len] = '\0';
      return (exprtk_value_t){EXPRTK_VAL_STRING,
                              .data.string = tstr_v_from_buf(buf, resp->body_len)};
    }
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

static coro_socket_t *net_ctx_recreate_ws_client(net_ctx_t *ctx) {
  coro_context_t *cctx;

  if (!ctx) return NULL;
  if (ctx->ws_client) {
    coro_socket_destroy(ctx->ws_client);
    ctx->ws_client = NULL;
  }

  cctx = coro_context_current();
  if (!cctx) {
    net_set_error(ctx, "no coroutine context");
    return NULL;
  }

  ctx->ws_client = coro_socket_create_tcpv4(cctx);
  return ctx->ws_client;
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
        exprtk_list_push(&list, net_json_to_exprtk_value(ud, turbo_json_array_get(value, i)));
      }
      return list;
    }
    case TURBO_JSON_OBJECT: {
      exprtk_value_t map = exprtk_val_map();
      size_t count = turbo_json_object_size(value);
      for (i = 0; i < count; i++) {
        const char *key = turbo_json_object_key(value, i);
        exprtk_map_set(&map, key ? key : "",
                       net_json_to_exprtk_value(ud, turbo_json_object_value(value, i)));
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
      exprtk_map_set(&headers, name, net_make_string_value(ud->env, value, strlen(value)));
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

  exprtk_map_set(&result, "status", exprtk_val_num(0.0));
  exprtk_map_set(&result, "body", net_make_string_value(ud->env, "", 0));
  exprtk_map_set(&result, "headers", exprtk_val_map());
  exprtk_map_set(&result, "data", net_null_value());
  if (error && error[0]) {
    exprtk_map_set(&result, "error", net_make_string_value(ud->env, error, strlen(error)));
  } else {
    exprtk_map_set(&result, "error", net_null_value());
  }

  return result;
}

static exprtk_value_t net_response_value(http_ud_t *ud, http_response_t *resp) {
  exprtk_value_t result = exprtk_val_map();

  exprtk_map_set(&result, "status", exprtk_val_num(resp ? (double)resp->status_code : 0.0));
  exprtk_map_set(&result, "body",
                 net_make_string_value(ud->env, resp && resp->body ? resp->body : "",
                                       resp && resp->body ? resp->body_len : 0));
  exprtk_map_set(&result, "headers", net_response_headers_map(ud, resp));
  exprtk_map_set(&result, "data", net_response_data_value(ud, resp));
  if (resp && resp->error && resp->error[0]) {
    exprtk_map_set(&result, "error", net_make_string_value(ud->env, resp->error, strlen(resp->error)));
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

static void net_apply_request_options(http_ud_t *ud, http_client_t *client,
                                      const exprtk_value_t *options,
                                      const char ***headers_out, int *header_count_out) {
  exprtk_value_t value;
  int bool_value;

  *headers_out = NULL;
  *header_count_out = 0;
  if (!options || options->type != EXPRTK_VAL_MAP) return;

  value = exprtk_map_get(options, "timeout");
  if (value.type == EXPRTK_VAL_NUMBER) {
    http_client_set_timeout(client, (int)value.data.number);
  } else if (value.type == EXPRTK_VAL_INTEGER) {
    http_client_set_timeout(client, (int)value.data.integer);
  }

  value = exprtk_map_get(options, "follow_redirects");
  if (net_truthy_value(&value, &bool_value)) {
    http_client_follow_redirects(client, bool_value);
  }

  value = exprtk_map_get(options, "headers");
  if (value.type == EXPRTK_VAL_MAP) {
    if (!net_build_request_headers(ud, &value, headers_out, header_count_out)) {
      *headers_out = NULL;
      *header_count_out = 0;
    }
  }

  value = exprtk_map_get(options, "basic_auth");
  if (value.type == EXPRTK_VAL_MAP) {
    exprtk_value_t user = exprtk_map_get(&value, "user");
    exprtk_value_t pass = exprtk_map_get(&value, "pass");
    if (user.type == EXPRTK_VAL_STRING && pass.type == EXPRTK_VAL_STRING) {
      http_client_set_basic_auth(client, net_arena_cstr(ud->scratch, user.data.string),
                                 net_arena_cstr(ud->scratch, pass.data.string));
    }
  }

  value = exprtk_map_get(options, "bearer_token");
  if (value.type == EXPRTK_VAL_STRING) {
    http_client_set_bearer_token(client, net_arena_cstr(ud->scratch, value.data.string));
  }
}

static exprtk_value_t net_http_request(http_ud_t *ud, http_method_t method, tstr_v url_sv,
                                       const tstr_v *body_sv, const exprtk_value_t *options,
                                       int structured_response) {
  http_client_t *client;
  http_response_t *resp;
  const char **headers = NULL;
  int header_count = 0;
  char *url;
  const char *body = NULL;
  size_t body_len = 0;
  exprtk_value_t ret = NET_ZERO;

  if (!ud || !ud->env) return NET_ZERO;

  client = http_client_create(NULL);
  if (!client) {
    return structured_response ? net_error_response_value(ud, "HTTP client create failed")
                               : NET_ZERO;
  }

  url = net_arena_cstr(ud->scratch, url_sv);
  if (!url) {
    http_client_destroy(client);
    return structured_response ? net_error_response_value(ud, "URL allocation failed")
                               : NET_ZERO;
  }

  if (body_sv) {
    body = body_sv->data;
    body_len = body_sv->len;
  }

  net_apply_request_options(ud, client, options, &headers, &header_count);
  resp = http_request(client, method, url, headers, header_count, body, body_len);
  if (resp) {
    ret = structured_response ? net_response_value(ud, resp) : copy_response_body(resp, ud->env);
    http_response_free(resp);
  } else if (structured_response) {
    ret = net_error_response_value(ud, "HTTP request failed");
  }

  http_client_destroy(client);
  return ret;
}

static exprtk_value_t fn_http_get(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;

  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    return net_http_request(ud, HTTP_GET, args[0].data.string, NULL, NULL, 0);
  }
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_MAP) {
    return net_http_request(ud, HTTP_GET, args[0].data.string, NULL, &args[1], 1);
  }

  return NET_ZERO;
}

static exprtk_value_t fn_http_post(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;

  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    return net_http_request(ud, HTTP_POST, args[0].data.string, &args[1].data.string, NULL, 0);
  }
  if (argc == 3 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING &&
      args[2].type == EXPRTK_VAL_MAP) {
    return net_http_request(ud, HTTP_POST, args[0].data.string, &args[1].data.string, &args[2], 1);
  }

  return NET_ZERO;
}

/* == Script functions: WebSocket ========================================= */

static exprtk_value_t fn_ws_connect(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
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

  client = net_ctx_recreate_ws_client(ud->ctx);
  if (!client) {
    return NET_ZERO;
  }

  url = net_arena_cstr(ud->scratch, args[0].data.string);
  if (!url) {
    net_set_error(ud->ctx, "url alloc failed");
    return NET_ZERO;
  }

  if (net_parse_ws_url(url, &host, &host_len, &port, &path, &is_tls) != 0) {
    net_set_error(ud->ctx, "invalid ws url");
    return NET_ZERO;
  }

  host_buf = mem_alloc(ud->scratch, host_len + 1);
  if (!host_buf) {
    net_set_error(ud->ctx, "host alloc failed");
    return NET_ZERO;
  }
  memcpy(host_buf, host, host_len);
  host_buf[host_len] = '\0';

  coro_socket_set_timeout(client, 10000);
  r = coro_socket_connect_ws(client, host_buf, port, path, is_tls);
  if (r != 0) {
    net_set_error(ud->ctx, "ws connect failed");
    return NET_ZERO;
  }

  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static exprtk_value_t fn_ws_send(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  tstr_v payload;
  int r;

  if (!ud || !ud->ctx || argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    return NET_ZERO;
  }
  if (!ud->ctx->ws_client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }

  payload = args[0].data.string;
  r = coro_socket_send(ud->ctx->ws_client, payload.data, payload.len);
  if (r < 0) {
    net_set_error(ud->ctx, "ws send failed");
    return NET_ZERO;
  }

  net_set_error(ud->ctx, "");
  return NET_ONE;
}

static exprtk_value_t fn_ws_recv(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  int timeout_ms = 5000;
  char *resp = NULL;
  size_t len = 0;
  int r;
  char *buf;

  if (!ud || !ud->ctx || argc > 1) return NET_ZERO;
  if (argc == 1 && args[0].type != EXPRTK_VAL_NUMBER) return NET_ZERO;
  if (!ud->ctx->ws_client) {
    net_set_error(ud->ctx, "ws client not connected");
    return NET_ZERO;
  }

  if (argc == 1) timeout_ms = (int)args[0].data.number;

  coro_socket_set_timeout(ud->ctx->ws_client, (uint64_t)timeout_ms);

  r = coro_socket_recv(ud->ctx->ws_client, &resp, &len);
  if (r != 0 || !resp) {
    net_set_error(ud->ctx, "ws recv failed or timeout");
    if (resp) coro_socket_free_recv(resp);
    return NET_ZERO;
  }

  buf = mem_alloc(&ud->env->arena, len + 1);
  if (!buf) {
    coro_socket_free_recv(resp);
    net_set_error(ud->ctx, "response alloc failed");
    return NET_ZERO;
  }
  memcpy(buf, resp, len);
  buf[len] = '\0';
  coro_socket_free_recv(resp);
  net_set_error(ud->ctx, "");
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t fn_ws_close(size_t argc, exprtk_value_t *args, void *user_data) {
  http_ud_t *ud = (http_ud_t *)user_data;
  (void)args;
  if (!ud || !ud->ctx || argc != 0) return NET_ZERO;

  if (ud->ctx->ws_client) {
    coro_socket_destroy(ud->ctx->ws_client);
    ud->ctx->ws_client = NULL;
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
  exprtk_env_register_func(env, "ws.recv", fn_ws_recv, ud);
  exprtk_env_register_func(env, "ws.close", fn_ws_close, ud);
}
