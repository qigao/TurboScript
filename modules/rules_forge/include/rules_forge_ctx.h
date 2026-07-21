/**
 * @file rules_forge_ctx.h
 * @brief TurboScript RulesForge plugin context.
 */
#ifndef RULES_FORGE_CTX_H
#define RULES_FORGE_CTX_H

#include "exprtk.h"
#include "rules_forge.h"
#include "turbo_buffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RFG_MAX_HANDLES 64

typedef struct {
    ruleforge_knowledge_base_t ptr;
} rfg_kb_slot_t;

typedef struct {
    ruleforge_stateful_session_t ptr;
    int kb_handle;
} rfg_session_slot_t;

typedef struct {
    ruleforge_query_result_t ptr;
    int session_handle;
} rfg_query_slot_t;

typedef struct {
    ruleforge_data_bind_stream_t ptr;
    int session_handle;
} rfg_stream_slot_t;

typedef struct {
    ruleforge_data_bind_object_t ptr;
} rfg_data_bind_object_slot_t;

typedef struct {
    ruleforge_continuous_session_t ptr;
    int kb_handle;
} rfg_continuous_slot_t;

typedef struct {
    ruleforge_continuous_result_t ptr;
    int continuous_handle;
} rfg_continuous_result_slot_t;

typedef struct {
    ruleforge_continuous_data_bind_stream_t ptr;
    int continuous_handle;
} rfg_continuous_stream_slot_t;

typedef struct {
    ruleforge_fact_t ptr;
    int owner_kind;   /* 1=session, 2=query, 3=continuous result */
    int owner_handle;
} rfg_fact_slot_t;

typedef struct {
    rfg_kb_slot_t kb[RFG_MAX_HANDLES];
    rfg_session_slot_t sessions[RFG_MAX_HANDLES];
    rfg_query_slot_t queries[RFG_MAX_HANDLES];
    rfg_stream_slot_t streams[RFG_MAX_HANDLES];
    rfg_data_bind_object_slot_t data_bind_objects[RFG_MAX_HANDLES];
    rfg_continuous_slot_t continuous[RFG_MAX_HANDLES];
    rfg_continuous_result_slot_t continuous_results[RFG_MAX_HANDLES];
    rfg_continuous_stream_slot_t continuous_streams[RFG_MAX_HANDLES];
    rfg_fact_slot_t facts[RFG_MAX_HANDLES];
    int initialized;
    char error_msg[512];
} rfg_ctx_t;

typedef struct {
    rfg_ctx_t *ctx;
    exprtk_env_t *env;
    mem_pool_t *scratch;
} rfg_ud_t;

void *rfg_ctx_create(void);
void rfg_ctx_destroy(void *ctx);
void rfg_plugin_load(void *ctx, void *env, void *scratch);

#endif /* RULES_FORGE_CTX_H */
