/**
 * @file test_net_task_http.c
 * @brief Managed-task HTTP facade transport tests.
 */
#include <iris/iris_app.h>
#include "platform.h"
#include <iris/server.h>
#include "tinytest.h"
#include "turbo_coro_context.h"
#include "turbo_coro_socket.h"
#include "turbo_script.h"

#include <stdio.h>
#include <string.h>

enum {
  NET_TASK_HTTP_TIMEOUT_MS = 5000,
  NET_TASK_HTTP_REQUEST_TIMEOUT_MS = 2000,
  NET_TASK_HTTP_PORT_FIRST = 39201,
  NET_TASK_HTTP_PORT_LAST = 39300
};

static void net_task_http_ok(Req *req, Res *res) {
  (void)req;
  reply(res, 200, "text/plain", "facade-ok", strlen("facade-ok"));
}

static coro_socket_t *net_task_http_listen(iris_app_t *app,
                                           coro_context_t *coro_ctx,
                                           int *port_out) {
  int port;
  for (port = NET_TASK_HTTP_PORT_FIRST; port <= NET_TASK_HTTP_PORT_LAST; ++port) {
    coro_socket_t *server = iris_server_start(app, coro_ctx, (unsigned short)port);
    if (server) {
      *port_out = port;
      return server;
    }
  }
  return NULL;
}

static void net_task_http_run_until(coro_context_t *coro_ctx,
                                    turbo_script_ctx_t *script_ctx,
                                    uint64_t timeout_ms) {
  uint64_t deadline = turbo_monotonic_ms() + timeout_ms;
  while (turbo_script_task_active_count(script_ctx) > 0 &&
         turbo_monotonic_ms() < deadline) {
    (void)coro_context_run(coro_ctx, TURBO_RUN_ONCE);
  }
}

static void net_task_http_stop_server(coro_context_t *coro_ctx,
                                      coro_socket_t *server) {
  uint64_t deadline = turbo_monotonic_ms() + NET_TASK_HTTP_TIMEOUT_MS;
  (void)coro_socket_server_stop(server);
  while (!coro_socket_server_is_stopped(server) &&
         turbo_monotonic_ms() < deadline) {
    (void)coro_context_run(coro_ctx, TURBO_RUN_NOWAIT);
  }
}

spec("net_task_http") {
  describe("TurboHttp facade transport") {
    it("routes an explicit HTTP/1 request") {
      coro_context_t *coro_ctx = coro_context_create(NULL);
      turbo_script_ctx_t *script_ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      iris_app_t *app = iris_app_create();
      coro_socket_t *server;
      char script[768];
      int port;

      check_not_null(coro_ctx);
      check_not_null(script_ctx);
      check_not_null(app);
      if (!coro_ctx || !script_ctx || !app) return;

      iris_app_get(app, "/ok", net_task_http_ok);
      server = net_task_http_listen(app, coro_ctx, &port);
      check_not_null(server);
      if (!server) {
        iris_app_destroy(app);
        turbo_script_free(script_ctx);
        coro_context_destroy(coro_ctx);
        return;
      }

      turbo_script_set_coro_context(script_ctx, coro_ctx);
      check_int_eq(turbo_script_load_plugin(script_ctx, "net"), 0);
      snprintf(script, sizeof(script),
               "request_task = task.spawn(() => "
               "  http.get(\"http://127.0.0.1:%d/ok\", "
               "           map { timeout: %d, transport: \"h1\" })"
               ");",
               port, NET_TASK_HTTP_REQUEST_TIMEOUT_MS);
      check_int_eq(turbo_script_run(script_ctx, script), 0);
      net_task_http_run_until(coro_ctx, script_ctx, NET_TASK_HTTP_TIMEOUT_MS);
      check_size_eq(turbo_script_task_active_count(script_ctx), 0);
      check_size_eq(turbo_script_task_failed_count(script_ctx), 0);
      check_int_eq(turbo_script_run(script_ctx,
                                    "response = task.result(request_task);"
                                    "response_status = response.status;"
                                    "response_body = response.body;"),
                   0);
      check_float_eq(ts_get_num(script_ctx, "response_status"), 200.0, 0.001);
      check_str_eq(ts_get_str(script_ctx, "response_body"), "facade-ok");

      net_task_http_stop_server(coro_ctx, server);
      check_true(coro_socket_server_is_stopped(server));
      coro_socket_destroy(server);
      iris_app_destroy(app);
      turbo_script_free(script_ctx);
      coro_context_destroy(coro_ctx);
    }

    it("routes an explicit HTTP/2 request") {
      coro_context_t *coro_ctx = coro_context_create(NULL);
      turbo_script_ctx_t *script_ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      iris_app_t *app = iris_app_create();
      coro_socket_t *server;
      char script[768];
      int port;

      check_not_null(coro_ctx);
      check_not_null(script_ctx);
      check_not_null(app);
      if (!coro_ctx || !script_ctx || !app) return;

      iris_app_get(app, "/ok", net_task_http_ok);
      server = net_task_http_listen(app, coro_ctx, &port);
      check_not_null(server);
      if (!server) {
        iris_app_destroy(app);
        turbo_script_free(script_ctx);
        coro_context_destroy(coro_ctx);
        return;
      }

      turbo_script_set_coro_context(script_ctx, coro_ctx);
      check_int_eq(turbo_script_load_plugin(script_ctx, "net"), 0);
      snprintf(script, sizeof(script),
               "request_task = task.spawn(() => "
               "  http.get(\"http://127.0.0.1:%d/ok\", "
               "           map { timeout: %d, transport: \"h2\" })"
               ");",
               port, NET_TASK_HTTP_REQUEST_TIMEOUT_MS);
      check_int_eq(turbo_script_run(script_ctx, script), 0);
      net_task_http_run_until(coro_ctx, script_ctx, NET_TASK_HTTP_TIMEOUT_MS);
      check_size_eq(turbo_script_task_active_count(script_ctx), 0);
      check_size_eq(turbo_script_task_failed_count(script_ctx), 0);
      check_int_eq(turbo_script_run(script_ctx,
                                    "response = task.result(request_task);"
                                    "response_status = response.status;"
                                    "response_body = response.body;"),
                   0);
      check_float_eq(ts_get_num(script_ctx, "response_status"), 200.0, 0.001);
      check_str_eq(ts_get_str(script_ctx, "response_body"), "facade-ok");

      net_task_http_stop_server(coro_ctx, server);
      check_true(coro_socket_server_is_stopped(server));
      coro_socket_destroy(server);
      iris_app_destroy(app);
      turbo_script_free(script_ctx);
      coro_context_destroy(coro_ctx);
    }
  }
}
