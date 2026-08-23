/**
 * @file wasm_ctx.h
 * @brief Wasm plugin context — handle table + error reporting.
 */
#ifndef WASM_CTX_H
#define WASM_CTX_H

#include "turbo_buffer.h"
#include "exprtk.h"
#include "turbo_wasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WASM_MAX_HANDLES 16

typedef struct {
  turbo_wasm_vm_t *vm;
  char error_msg[256];
} wasm_handle_t;

typedef struct {
  wasm_handle_t *handles[WASM_MAX_HANDLES];
  char last_error[256];
} wasm_ctx_t;

typedef struct {
  wasm_ctx_t *ctx;
  exprtk_env_t *env;
  mem_pool_t *scratch;
} wasm_ud_t;

#define WASM_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

#define WASM_CTX_ABORT(ud)                                                                          \
  do {                                                                                              \
    (ud)->env->aborted = 1;                                                                         \
  } while (0)

static inline mem_pool_t *wasm_tmp_arena(wasm_ud_t *ud) {
  if (ud->scratch)
    return ud->scratch;
  return &ud->env->arena;
}

static inline char *wasm_arena_cstr(mem_pool_t *a, vstr sv) {
  char *buf = (char *)mem_alloc(a, sv.len + 1);
  if (!buf)
    return NULL;
  memcpy(buf, sv.data, sv.len);
  buf[sv.len] = '\0';
  return buf;
}

void *wasm_ctx_create(void);
void wasm_ctx_destroy(void *ctx);
void wasm_load(void *ctx, void *env, void *scratch);

#endif /* WASM_CTX_H */

