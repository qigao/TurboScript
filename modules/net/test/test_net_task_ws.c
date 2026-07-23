/**
 * @file test_net_task_ws.c
 * @brief Managed-task WebSocket connection isolation tests.
 */
#include "tinytest.h"
#include "turbo_coro_context.h"
#include "turbo_coro_socket.h"
#include "turbo_script.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

enum {
  NET_TASK_WS_TIMEOUT_MS = 5000,
  NET_TASK_WS_RECV_TIMEOUT_MS = 2000,
  NET_TASK_WS_HOLD_MS = 1000,
  NET_TASK_WS_PORT_FIRST = 39100,
  NET_TASK_WS_PORT_LAST = 39200
};

typedef struct {
  coro_context_t *coro_ctx;
  int handler_count;
  int market_count;
  int sports_count;
  int hold_count;
  int handler_errors;
} net_task_ws_server_t;

static void net_task_ws_handler(coro_socket_t *client, void *arg) {
  net_task_ws_server_t *state = (net_task_ws_server_t *)arg;
  char *data = NULL;
  size_t len = 0;
  const char *reply = NULL;

  state->handler_count++;
  coro_socket_set_timeout(client, NET_TASK_WS_RECV_TIMEOUT_MS);
  if (coro_socket_recv(client, &data, &len) != 0 || !data) {
    state->handler_errors++;
    return;
  }

  if (len == strlen("hold") && memcmp(data, "hold", len) == 0) {
    state->hold_count++;
    coro_socket_free_recv(data);
    coro_sleep(state->coro_ctx, NET_TASK_WS_HOLD_MS);
    return;
  } else if (len == strlen("market-subscribe") &&
             memcmp(data, "market-subscribe", len) == 0) {
    state->market_count++;
    reply = "market-ready";
  } else if (len == strlen("sports-subscribe") &&
             memcmp(data, "sports-subscribe", len) == 0) {
    state->sports_count++;
    reply = "sports-ready";
  } else {
    state->handler_errors++;
  }
  coro_socket_free_recv(data);

  if (reply && coro_socket_send(client, reply, strlen(reply)) != 0)
    state->handler_errors++;
}

static coro_socket_t *net_task_ws_listen(coro_context_t *coro_ctx,
                                         net_task_ws_server_t *state, int *port_out) {
  int port;
  for (port = NET_TASK_WS_PORT_FIRST; port <= NET_TASK_WS_PORT_LAST; ++port) {
    coro_socket_t *server = coro_socket_create_tcpv4(coro_ctx);
    if (!server) return NULL;
    if (coro_socket_listen_ws(server, "127.0.0.1", port, 0,
                              net_task_ws_handler, state) == 0) {
      *port_out = port;
      return server;
    }
    coro_socket_destroy(server);
  }
  return NULL;
}

static void net_task_ws_run_until(coro_context_t *coro_ctx, turbo_script_ctx_t *script_ctx,
                                  uint64_t timeout_ms) {
  uint64_t deadline = turbo_monotonic_ms() + timeout_ms;
  while (turbo_script_task_active_count(script_ctx) > 0 &&
         turbo_monotonic_ms() < deadline) {
    (void)coro_context_run(coro_ctx, TURBO_RUN_ONCE);
  }
}

static void net_task_ws_stop_server(coro_context_t *coro_ctx, coro_socket_t *server) {
  uint64_t deadline = turbo_monotonic_ms() + NET_TASK_WS_TIMEOUT_MS;
  (void)coro_socket_server_stop(server);
  while (!coro_socket_server_is_stopped(server) && turbo_monotonic_ms() < deadline)
    (void)coro_context_run(coro_ctx, TURBO_RUN_NOWAIT);
}

spec("net_task_ws") {
  describe("managed task connections") {
    it("keeps concurrent WebSocket clients isolated by task") {
      net_task_ws_server_t state = {0};
      coro_context_t *coro_ctx = coro_context_create(NULL);
      turbo_script_ctx_t *script_ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      coro_socket_t *server;
      char script[2048];
      int port;

      check_not_null(coro_ctx);
      check_not_null(script_ctx);
      if (!coro_ctx || !script_ctx) return;

      state.coro_ctx = coro_ctx;
      turbo_script_set_coro_context(script_ctx, coro_ctx);
      check_int_eq(turbo_script_load_plugin(script_ctx, "net"), 0);

      server = net_task_ws_listen(coro_ctx, &state, &port);
      check_not_null(server);
      if (!server) {
        turbo_script_free(script_ctx);
        coro_context_destroy(coro_ctx);
        return;
      }
      check_int_lt(0, port);

      snprintf(script, sizeof(script),
               "market_task = task.spawn(() => {"
               "  if (ws.connect(\"ws://127.0.0.1:%d/market\") == 0) throw \"market connect\";"
               "  if (ws.send(\"market-subscribe\") == 0) throw \"market send\";"
               "  var reply = ws.consume(%d, (message) => message); ws.close(); return reply;"
               "});"
               "sports_task = task.spawn(() => {"
               "  if (ws.connect(\"ws://127.0.0.1:%d/sports\") == 0) throw \"sports connect\";"
               "  if (ws.send(\"sports-subscribe\") == 0) throw \"sports send\";"
               "  var reply = ws.consume(%d, (message) => message); ws.close(); return reply;"
               "});",
               port, NET_TASK_WS_RECV_TIMEOUT_MS, port, NET_TASK_WS_RECV_TIMEOUT_MS);

      check_int_eq(turbo_script_run(script_ctx, script), 0);
      net_task_ws_run_until(coro_ctx, script_ctx, NET_TASK_WS_TIMEOUT_MS);
      check_size_eq(turbo_script_task_active_count(script_ctx), 0);
      check_size_eq(turbo_script_task_failed_count(script_ctx), 0);
      check_int_eq(turbo_script_run(script_ctx,
                                    "market_reply = task.result(market_task);"
                                    "sports_reply = task.result(sports_task);"),
                   0);
      check_str_eq(ts_get_str(script_ctx, "market_reply"), "market-ready");
      check_str_eq(ts_get_str(script_ctx, "sports_reply"), "sports-ready");
      check_int_eq(state.handler_count, 2);
      check_int_eq(state.market_count, 1);
      check_int_eq(state.sports_count, 1);
      check_int_eq(state.handler_errors, 0);

      net_task_ws_stop_server(coro_ctx, server);
      check_true(coro_socket_server_is_stopped(server));
      coro_socket_destroy(server);
      turbo_script_free(script_ctx);
      coro_context_destroy(coro_ctx);
    }

    it("cancels a task waiting in ws.consume") {
      net_task_ws_server_t state = {0};
      coro_context_t *coro_ctx = coro_context_create(NULL);
      turbo_script_ctx_t *script_ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      coro_socket_t *server;
      char script[1536];
      int port;

      check_not_null(coro_ctx);
      check_not_null(script_ctx);
      if (!coro_ctx || !script_ctx) return;

      state.coro_ctx = coro_ctx;
      turbo_script_set_coro_context(script_ctx, coro_ctx);
      check_int_eq(turbo_script_load_plugin(script_ctx, "net"), 0);
      server = net_task_ws_listen(coro_ctx, &state, &port);
      check_not_null(server);
      if (!server) {
        turbo_script_free(script_ctx);
        coro_context_destroy(coro_ctx);
        return;
      }

      snprintf(script, sizeof(script),
               "waiting_task = task.spawn(() => {"
               "  if (ws.connect(\"ws://127.0.0.1:%d/hold\") == 0) throw \"hold connect\";"
               "  if (ws.send(\"hold\") == 0) throw \"hold send\";"
               "  var reply = ws.consume(%d, (message) => message); ws.close(); return reply;"
               "});"
               "cancel_task = task.spawn(() => {"
               "  task.sleep(20); return task.cancel(waiting_task);"
               "});",
               port, NET_TASK_WS_TIMEOUT_MS);

      check_int_eq(turbo_script_run(script_ctx, script), 0);
      net_task_ws_run_until(coro_ctx, script_ctx, NET_TASK_WS_TIMEOUT_MS);
      check_size_eq(turbo_script_task_active_count(script_ctx), 0);
      check_size_eq(turbo_script_task_failed_count(script_ctx), 0);
      check_int_eq(turbo_script_run(script_ctx,
                                    "cancel_result = task.result(cancel_task);"
                                    "waiting_state = task.status(waiting_task);"),
                   0);
      check_float_eq(ts_get_num(script_ctx, "cancel_result"), 1.0, 0.001);
      check_str_eq(ts_get_str(script_ctx, "waiting_state"), "cancelled");
      check_int_eq(state.handler_count, 1);
      check_int_eq(state.hold_count, 1);
      check_int_eq(state.handler_errors, 0);

      net_task_ws_stop_server(coro_ctx, server);
      check_true(coro_socket_server_is_stopped(server));
      coro_socket_destroy(server);
      turbo_script_free(script_ctx);
      coro_context_destroy(coro_ctx);
    }

    it("fails a task when a frame exceeds the external value quota") {
      net_task_ws_server_t state = {0};
      coro_context_t *coro_ctx = coro_context_create(NULL);
      turbo_script_ctx_t *script_ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_memory_policy_t policy;
      coro_socket_t *server;
      char script[1024];
      int port;

      check_not_null(coro_ctx);
      check_not_null(script_ctx);
      if (!coro_ctx || !script_ctx) return;

      check_int_eq(turbo_script_memory_policy_init(TURBO_SCRIPT_MEMORY_STREAMING, &policy), 0);
      policy.max_external_value_bytes = 4;
      check_int_eq(turbo_script_set_memory_policy(script_ctx, &policy), 0);
      state.coro_ctx = coro_ctx;
      turbo_script_set_coro_context(script_ctx, coro_ctx);
      check_int_eq(turbo_script_load_plugin(script_ctx, "net"), 0);
      server = net_task_ws_listen(coro_ctx, &state, &port);
      check_not_null(server);
      if (!server) {
        turbo_script_free(script_ctx);
        coro_context_destroy(coro_ctx);
        return;
      }

      snprintf(script, sizeof(script),
               "quota_task = task.spawn(() => {"
               "  if (ws.connect(\"ws://127.0.0.1:%d/market\") == 0) throw \"connect\";"
               "  if (ws.send(\"market-subscribe\") == 0) throw \"send\";"
               "  return ws.consume(%d, (message) => message);"
               "});",
               port, NET_TASK_WS_RECV_TIMEOUT_MS);

      check_int_eq(turbo_script_run(script_ctx, script), 0);
      net_task_ws_run_until(coro_ctx, script_ctx, NET_TASK_WS_TIMEOUT_MS);
      check_size_eq(turbo_script_task_active_count(script_ctx), 0);
      check_size_eq(turbo_script_task_failed_count(script_ctx), 1);
      check_int_eq(turbo_script_run(script_ctx, "quota_state = task.status(quota_task);"), 0);
      check_str_eq(ts_get_str(script_ctx, "quota_state"), "failed");

      net_task_ws_stop_server(coro_ctx, server);
      check_true(coro_socket_server_is_stopped(server));
      coro_socket_destroy(server);
      turbo_script_free(script_ctx);
      coro_context_destroy(coro_ctx);
    }
  }
}
