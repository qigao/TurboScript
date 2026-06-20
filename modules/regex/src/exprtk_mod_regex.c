/**
 * @file exprtk_mod_regex.c
 * @brief Regex module functions for TurboScript
 */
#include "regex_ctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fsm/fsm.h>
#include <re/re.h> 

#define REGEX_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

#define REGEX_ERROR(ud, msg)                                              \
    do {                                                                  \
        if ((ud) && (ud)->ctx) {                                          \
            strncpy((ud)->ctx->error_msg, (msg),                          \
                    sizeof((ud)->ctx->error_msg) - 1);                    \
            (ud)->ctx->error_msg[sizeof((ud)->ctx->error_msg) - 1] = '\0'; \
        }                                                                 \
        if ((ud) && (ud)->env) {                                          \
            (ud)->env->aborted = 1;                                       \
        }                                                                 \
    } while (0)

/* Helper: convert string view to C string using scratch arena */
static inline char *regex_arena_cstr(mem_pool_t *arena, tstr_v sv) {
    char *buf = (char *)mem_alloc(arena, sv.len + 1);
    if (buf) {
        memcpy(buf, sv.data, sv.len);
        buf[sv.len] = '\0';
    }
    return buf;
}

/* Helper: allocate pattern handle */
static int regex_alloc_pattern(regex_ctx_t *ctx, regex_pattern_t *pattern) {
    for (int i = 0; i < REGEX_MAX_PATTERNS; i++) {
        if (!ctx->patterns[i]) {
            ctx->patterns[i] = pattern;
            return i;
        }
    }
    return -1; /* No free slot */
}

/* Helper: free pattern handle */
static void regex_free_pattern(regex_ctx_t *ctx, int handle) {
    if (handle < 0 || handle >= REGEX_MAX_PATTERNS) return;
    
    if (ctx->patterns[handle]) {
        if (ctx->patterns[handle]->fsm) {
            fsm_free(ctx->patterns[handle]->fsm);
        }
        if (ctx->patterns[handle]->pattern_str) {
            free(ctx->patterns[handle]->pattern_str);
        }
        free(ctx->patterns[handle]);
        ctx->patterns[handle] = NULL;
    }
}

/* =========================================================================
 * Regex Functions
 * ========================================================================= */

/**
 * regex.compile(pattern: string) -> int
 * 编译正则表达式，返回模式句柄
 */
static exprtk_value_t fn_regex_compile(size_t argc, exprtk_value_t *args, void *user_data) {
    regex_ud_t *ud = (regex_ud_t *)user_data;
    
    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
        REGEX_ERROR(ud, "regex.compile: expected string argument");
        return REGEX_ZERO;
    }
    
    char *pattern = regex_arena_cstr(ud->scratch, args[0].data.string);
    if (!pattern) {
        REGEX_ERROR(ud, "regex.compile: out of memory");
        return REGEX_ZERO;
    }
    
    /* Parse regex using libfsm */
    enum re_flags flags = 0;  /* Default flags */
    struct re_err err;
    
    struct fsm *fsm = re_comp(RE_PCRE, fsm_sgetc, &pattern, NULL, flags, &err);
    
    if (!fsm) {
        REGEX_ERROR(ud, "regex.compile: pattern compilation failed");
        return REGEX_ZERO;
    }
    
    /* Minimize FSM for better performance */
    if (!fsm_minimise(fsm)) {
        fsm_free(fsm);
        REGEX_ERROR(ud, "regex.compile: FSM minimization failed");
        return REGEX_ZERO;
    }
    
    /* Allocate pattern structure */
    regex_pattern_t *ptn = (regex_pattern_t *)malloc(sizeof(regex_pattern_t));
    if (!ptn) {
        fsm_free(fsm);
        REGEX_ERROR(ud, "regex.compile: out of memory");
        return REGEX_ZERO;
    }
    
    ptn->fsm = fsm;
    ptn->pattern_str = strdup(pattern);
    
    /* Allocate handle */
    int handle = regex_alloc_pattern(ud->ctx, ptn);
    if (handle < 0) {
        fsm_free(fsm);
        free(ptn->pattern_str);
        free(ptn);
        REGEX_ERROR(ud, "regex.compile: too many patterns (max 32)");
        return REGEX_ZERO;
    }
    
    return exprtk_val_int(handle);
}

/**
 * regex.match(handle: int, text: string) -> int
 * 检查文本是否匹配模式（完全匹配）
 */
static exprtk_value_t fn_regex_match(size_t argc, exprtk_value_t *args, void *user_data) {
    regex_ud_t *ud = (regex_ud_t *)user_data;
    
    if (argc < 2 || (args[0].type != EXPRTK_VAL_INTEGER && args[0].type != EXPRTK_VAL_NUMBER) 
        || args[1].type != EXPRTK_VAL_STRING) {
        REGEX_ERROR(ud, "regex.match: expected (int, string)");
        return REGEX_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    if (handle < 0 || handle >= REGEX_MAX_PATTERNS || !ud->ctx->patterns[handle]) {
        REGEX_ERROR(ud, "regex.match: invalid pattern handle");
        return REGEX_ZERO;
    }
    
    char *text = regex_arena_cstr(ud->scratch, args[1].data.string);
    if (!text) {
        REGEX_ERROR(ud, "regex.match: out of memory");
        return REGEX_ZERO;
    }
    
    struct fsm *fsm = ud->ctx->patterns[handle]->fsm;
    
    /* Execute FSM */
    fsm_state_t end_state;
    int result = fsm_exec(fsm, fsm_sgetc, &text, &end_state, NULL);
    
    /* Check if we reached an end state */
    int is_match = (result == 1) ? 1 : 0;
    
    return exprtk_val_num((double)is_match);
}

/**
 * regex.test(pattern: string, text: string) -> int
 * 一次性测试（不编译缓存）
 */
static exprtk_value_t fn_regex_test(size_t argc, exprtk_value_t *args, void *user_data) {
    regex_ud_t *ud = (regex_ud_t *)user_data;
    
    if (argc < 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        REGEX_ERROR(ud, "regex.test: expected (string, string)");
        return REGEX_ZERO;
    }
    
    char *pattern = regex_arena_cstr(ud->scratch, args[0].data.string);
    char *text = regex_arena_cstr(ud->scratch, args[1].data.string);
    
    if (!pattern || !text) {
        REGEX_ERROR(ud, "regex.test: out of memory");
        return REGEX_ZERO;
    }
    
    /* Compile pattern */
    enum re_flags flags = 0;
    struct re_err err;
    
    struct fsm *fsm = re_comp(RE_PCRE, fsm_sgetc, &pattern, NULL, flags, &err);
    
    if (!fsm) {
        REGEX_ERROR(ud, "regex.test: pattern compilation failed");
        return REGEX_ZERO;
    }
    
    /* Minimize and execute */
    fsm_minimise(fsm);
    
    fsm_state_t end_state;
    int result = fsm_exec(fsm, fsm_sgetc, &text, &end_state, NULL);
    int is_match = (result == 1) ? 1 : 0;
    
    fsm_free(fsm);
    
    return exprtk_val_num((double)is_match);
}

/**
 * regex.search(handle: int, text: string) -> int
 * 在文本中搜索模式（部分匹配）
 */
static exprtk_value_t fn_regex_search(size_t argc, exprtk_value_t *args, void *user_data) {
    regex_ud_t *ud = (regex_ud_t *)user_data;
    
    if (argc < 2 || (args[0].type != EXPRTK_VAL_INTEGER && args[0].type != EXPRTK_VAL_NUMBER) 
        || args[1].type != EXPRTK_VAL_STRING) {
        REGEX_ERROR(ud, "regex.search: expected (int, string)");
        return REGEX_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    if (handle < 0 || handle >= REGEX_MAX_PATTERNS || !ud->ctx->patterns[handle]) {
        REGEX_ERROR(ud, "regex.search: invalid pattern handle");
        return REGEX_ZERO;
    }
    
    tstr_v text_sv = args[1].data.string;
    struct fsm *fsm = ud->ctx->patterns[handle]->fsm;
    
    /* Search for pattern in text */
    for (size_t i = 0; i < text_sv.len; i++) {
        const char *substr = text_sv.data + i;
        fsm_state_t end_state;
        int result = fsm_exec(fsm, fsm_sgetc, &substr, &end_state, NULL);
        
        if (result == 1) {
            return exprtk_val_num((double)i);  /* Return position */
        }
    }
    
    return exprtk_val_num(-1.0);  /* Not found */
}

/**
 * regex.free(handle: int) -> int
 * 释放编译的模式
 */
static exprtk_value_t fn_regex_free(size_t argc, exprtk_value_t *args, void *user_data) {
    regex_ud_t *ud = (regex_ud_t *)user_data;
    
    if (argc < 1 || (args[0].type != EXPRTK_VAL_INTEGER && args[0].type != EXPRTK_VAL_NUMBER)) {
        return REGEX_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    regex_free_pattern(ud->ctx, handle);
    return exprtk_val_num(1.0);
}

/* =========================================================================
 * Module Registration
 * ========================================================================= */

void regex_load(void *p, void *e, void *s) {
    regex_ctx_t *ctx = (regex_ctx_t *)p;
    exprtk_env_t *env = (exprtk_env_t *)e;
    mem_pool_t *scratch = (mem_pool_t *)s;
    
    if (!ctx || !env) return;
    
    /* Allocate user data */
    regex_ud_t *ud = (regex_ud_t *)mem_alloc(&env->arena, sizeof(regex_ud_t));
    if (!ud) return;
    
    ud->ctx = ctx;
    ud->env = env;
    ud->scratch = scratch;
    
    /* Register regex functions */
    exprtk_env_register_func(env, "regex.compile", fn_regex_compile, ud);
    exprtk_env_register_func(env, "regex.match", fn_regex_match, ud);
    exprtk_env_register_func(env, "regex.test", fn_regex_test, ud);
    exprtk_env_register_func(env, "regex.search", fn_regex_search, ud);
    exprtk_env_register_func(env, "regex.free", fn_regex_free, ud);
}
