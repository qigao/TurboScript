#include "turbo_script_closure_analysis.h"
#include "exprtk.h"
#include <stdlib.h>
#include <string.h>

/* 内部数据结构：变量集合（用于记录使用/定义的变量） */
typedef struct {
  const char **names;
  size_t count;
  size_t capacity;
} ts_var_set_t;

/* 分析上下文 */
typedef struct {
  ts_var_set_t used;       // 使用的变量
  ts_var_set_t defined;    // 定义的变量（包括参数和局部）
  ts_var_set_t assigned;   // 赋值目标；需经外层绑定解析后才能捕获
  ts_var_set_t read_before_assignment; // 局部赋值前读取的变量
  int has_nested_closure;  // 是否有嵌套闭包
  int has_modification;    // 是否修改捕获变量
} ts_analysis_ctx_t;

/* =========================================================================
 *  变量集合操作
 * ========================================================================= */

static void ts_var_set_init(ts_var_set_t *set) {
  set->names = NULL;
  set->count = 0;
  set->capacity = 0;
}

static void ts_var_set_free(ts_var_set_t *set) {
  free(set->names);
  set->names = NULL;
  set->count = 0;
  set->capacity = 0;
}

static int ts_var_set_contains(const ts_var_set_t *set, const char *name) {
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], name) == 0) return 1;
  }
  return 0;
}

static void ts_var_set_add(ts_var_set_t *set, const char *name) {
  if (ts_var_set_contains(set, name)) return;
  
  if (set->count >= set->capacity) {
    size_t new_cap = set->capacity == 0 ? 8 : set->capacity * 2;
    const char **new_names = (const char **)realloc(set->names, new_cap * sizeof(const char *));
    if (!new_names) return; // OOM
    set->names = new_names;
    set->capacity = new_cap;
  }
  
  set->names[set->count++] = name;
}

/* =========================================================================
 *  AST 遍历与分析
 * ========================================================================= */

/* 前向声明 */
static void ts_analyze_node(exprtk_node_t *node, ts_analysis_ctx_t *ctx, int in_assignment_lhs);

/* 分析变量引用 */
static void ts_analyze_variable(exprtk_node_t *node, ts_analysis_ctx_t *ctx, int in_assignment_lhs) {
  const char *name = node->data.variable.name;
  
  if (in_assignment_lhs) {
    // 赋值左侧：记录为定义
    ts_var_set_add(&ctx->defined, name);
  } else {
    // 读取：记录为使用
    if (!ts_var_set_contains(&ctx->defined, name) &&
        !ts_var_set_contains(&ctx->assigned, name))
      ts_var_set_add(&ctx->read_before_assignment, name);
    ts_var_set_add(&ctx->used, name);
  }
}

/* 分析赋值 */
static void ts_analyze_assignment(exprtk_node_t *node, ts_analysis_ctx_t *ctx) {
  // 赋值值记为使用
  if (node->data.assignment.value) {
    ts_analyze_node(node->data.assignment.value, ctx, 0);
  }

  if (node->data.assignment.name)
    ts_var_set_add(&ctx->assigned, node->data.assignment.name);
}

/* Only direct module-scope bindings are definite before an exported function
 * executes.  Do not descend into function/control-flow bodies: assignments in
 * those scopes do not prove that exprtk_env_set() will find an outer binding. */
static int ts_outer_scope_defines_name(const exprtk_node_t *outer_scope,
                                       const char *name) {
  if (!outer_scope || !name) return 0;
  if (outer_scope->type == EXPRTK_NODE_ASSIGNMENT ||
      outer_scope->type == EXPRTK_NODE_CONSTANT_DECL)
    return outer_scope->data.assignment.name &&
           strcmp(outer_scope->data.assignment.name, name) == 0;
  if (outer_scope->type != EXPRTK_NODE_BLOCK) return 0;
  for (size_t i = 0; i < outer_scope->data.block.count; ++i) {
    const exprtk_node_t *statement = outer_scope->data.block.statements[i];
    if (statement &&
        (statement->type == EXPRTK_NODE_ASSIGNMENT ||
         statement->type == EXPRTK_NODE_CONSTANT_DECL) &&
        statement->data.assignment.name &&
        strcmp(statement->data.assignment.name, name) == 0)
      return 1;
  }
  return 0;
}

/* 分析函数定义（嵌套闭包检测） */
static void ts_analyze_func_def(exprtk_node_t *node, ts_analysis_ctx_t *ctx) {
  // 检测到嵌套函数定义
  ctx->has_nested_closure = 1;
  
  // 注：不递归分析嵌套函数体，因为它们有自己的作用域
}

/* 分析函数表达式（闭包/匿名函数） */
static void ts_analyze_func_expr(exprtk_node_t *node, ts_analysis_ctx_t *ctx) {
  // 检测到函数表达式（闭包）
  ctx->has_nested_closure = 1;
  
  // 注：函数表达式内部不分析，它们是独立的闭包
}

/* 分析节点（递归） */
static void ts_analyze_node(exprtk_node_t *node, ts_analysis_ctx_t *ctx, int in_assignment_lhs) {
  if (!node) return;
  
  switch (node->type) {
    case EXPRTK_NODE_VARIABLE:
      ts_analyze_variable(node, ctx, in_assignment_lhs);
      break;
    
    case EXPRTK_NODE_ASSIGNMENT:
      ts_analyze_assignment(node, ctx);
      break;
    
    case EXPRTK_NODE_FUNCTION_DEFINITION:
      ts_analyze_func_def(node, ctx);
      break;
    
    case EXPRTK_NODE_FUNCTION_EXPRESSION:
      ts_analyze_func_expr(node, ctx);
      break;
    
    case EXPRTK_NODE_BINARY_OP:
      ts_analyze_node(node->data.binary.left, ctx, 0);
      ts_analyze_node(node->data.binary.right, ctx, 0);
      break;
    
    case EXPRTK_NODE_FUNCTION_CALL:
      for (size_t i = 0; i < node->data.function.arg_count; i++) {
        ts_analyze_node(node->data.function.args[i], ctx, 0);
      }
      break;
    
    case EXPRTK_NODE_IF:
      ts_analyze_node(node->data.if_stmt.condition, ctx, 0);
      ts_analyze_node(node->data.if_stmt.if_branch, ctx, 0);
      ts_analyze_node(node->data.if_stmt.else_branch, ctx, 0);
      break;
    
    case EXPRTK_NODE_WHILE:
      ts_analyze_node(node->data.while_loop.condition, ctx, 0);
      ts_analyze_node(node->data.while_loop.body, ctx, 0);
      break;
    
    case EXPRTK_NODE_FOR:
      ts_analyze_node(node->data.for_loop.init, ctx, 0);
      ts_analyze_node(node->data.for_loop.condition, ctx, 0);
      ts_analyze_node(node->data.for_loop.post, ctx, 0);
      ts_analyze_node(node->data.for_loop.body, ctx, 0);
      break;
    
    case EXPRTK_NODE_BLOCK:
      for (size_t i = 0; i < node->data.block.count; i++) {
        ts_analyze_node(node->data.block.statements[i], ctx, 0);
      }
      break;
    
    case EXPRTK_NODE_FLOW:
      ts_analyze_node(node->data.flow.value, ctx, 0);
      break;
    
    case EXPRTK_NODE_VECTOR:
      for (size_t i = 0; i < node->data.vector.count; i++) {
        ts_analyze_node(node->data.vector.elements[i], ctx, 0);
      }
      break;
    
    case EXPRTK_NODE_INDEX:
      ts_analyze_node(node->data.index_access.array, ctx, 0);
      ts_analyze_node(node->data.index_access.index, ctx, 0);
      break;
    
    case EXPRTK_NODE_MEMBER_CALL:
      ts_analyze_node(node->data.member_call.object, ctx, 0);
      for (size_t i = 0; i < node->data.member_call.arg_count; i++) {
        ts_analyze_node(node->data.member_call.args[i], ctx, 0);
      }
      break;
    
    case EXPRTK_NODE_MEMBER_ACCESS:
      ts_analyze_node(node->data.member_access.object, ctx, 0);
      break;
    
    case EXPRTK_NODE_FOR_IN:
      // for_in 的变量是局部定义
      ts_var_set_add(&ctx->defined, node->data.for_in.var_name);
      ts_analyze_node(node->data.for_in.collection, ctx, 0);
      ts_analyze_node(node->data.for_in.body, ctx, 0);
      break;
    
    // 其他节点类型默认不包含变量引用或递归处理
    default:
      break;
  }
}

/* =========================================================================
 *  公开 API
 * ========================================================================= */

ts_closure_analysis_t *ts_analyze_closure(exprtk_node_t *func_body,
                                          exprtk_node_t **arg_params,
                                          size_t arg_count,
                                          exprtk_env_t *closure_env,
                                          exprtk_node_t *outer_scope) {
  // 初始化分析上下文
  ts_analysis_ctx_t ctx;
  ts_var_set_init(&ctx.used);
  ts_var_set_init(&ctx.defined);
  ts_var_set_init(&ctx.assigned);
  ts_var_set_init(&ctx.read_before_assignment);
  ctx.has_nested_closure = 0;
  ctx.has_modification = 0;
  
  // 标记函数参数为已定义
  for (size_t i = 0; i < arg_count; i++) {
    if (arg_params[i]->type == EXPRTK_NODE_VARIABLE) {
      ts_var_set_add(&ctx.defined, arg_params[i]->data.variable.name);
    } else if (arg_params[i]->type == EXPRTK_NODE_ASSIGNMENT) {
      // 带默认值的参数
      ts_var_set_add(&ctx.defined, arg_params[i]->data.assignment.name);
    }
  }
  
  // 遍历函数体，收集变量使用信息
  ts_analyze_node(func_body, &ctx, 0);
  
  // 计算自由变量（used - defined）
  ts_var_set_t free_vars;
  int has_unbound_read_before_assignment = 0;
  ts_var_set_init(&free_vars);
  
  for (size_t i = 0; i < ctx.used.count; i++) {
    const char *name = ctx.used.names[i];
    if (!ts_var_set_contains(&ctx.defined, name)) {
      if (ts_var_set_contains(&ctx.assigned, name) &&
          !exprtk_env_has(closure_env, name) &&
          !ts_outer_scope_defines_name(outer_scope, name)) {
        /* A local written before its first read has a stable MIR register.
         * Reading it first (x = x + 1) instead depends on exprtk's fresh
         * per-call function environment and must stay interpreted. */
        if (ts_var_set_contains(&ctx.read_before_assignment, name))
          has_unbound_read_before_assignment = 1;
        continue;
      }
      ts_var_set_add(&free_vars, name);
    }
  }
  
  // 构建分析结果
  ts_closure_analysis_t *result = (ts_closure_analysis_t *)malloc(sizeof(ts_closure_analysis_t));
  if (!result) {
    ts_var_set_free(&ctx.used);
    ts_var_set_free(&ctx.defined);
    ts_var_set_free(&ctx.assigned);
    ts_var_set_free(&ctx.read_before_assignment);
    ts_var_set_free(&free_vars);
    return NULL;
  }
  
  result->captured_count = free_vars.count;
  result->captured_vars = free_vars.names; // 转移所有权
  
  // 判断是否可以 JIT
  result->can_jit = 1;
  result->reason = NULL;
  
  if (has_unbound_read_before_assignment) {
    result->can_jit = 0;
    result->reason = "unbound assignment requires interpreter scope";
  } else if (ctx.has_nested_closure) {
    result->can_jit = 0;
    result->reason = "nested closure not supported";
  } else if (free_vars.count > 10) {
    result->can_jit = 0;
    result->reason = "too many captured variables (max 10)";
  } else if (free_vars.count == 0) {
    // 无捕获变量，不是真正的闭包，可以JIT
    result->can_jit = 1;
  }
  
  // 清理中间数据
  ts_var_set_free(&ctx.used);
  ts_var_set_free(&ctx.defined);
  ts_var_set_free(&ctx.assigned);
  ts_var_set_free(&ctx.read_before_assignment);
  // free_vars.names 已转移给 result，不释放
  
  return result;
}

void ts_closure_analysis_free(ts_closure_analysis_t *analysis) {
  if (!analysis) return;
  free(analysis->captured_vars);
  free(analysis);
}
