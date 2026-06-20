#ifndef TURBO_SCRIPT_CLOSURE_ANALYSIS_H
#define TURBO_SCRIPT_CLOSURE_ANALYSIS_H

#include "exprtk_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 闭包捕获变量信息
 */
typedef struct {
  const char **captured_vars; // 捕获的变量名列表（所有权归调用者）
  size_t captured_count;      // 捕获变量数量
  int can_jit;                // 是否可以预编译成独立 MIR 函数
  const char *reason;         // 不能预编译的原因（can_jit=0时有效）
} ts_closure_analysis_t;

/**
 * @brief 分析函数体中的自由变量（闭包捕获）
 * 
 * @param func_body 函数体节点
 * @param arg_params 函数参数节点数组
 * @param arg_count 参数数量
 * @param closure_env 闭包环境（父作用域）
 * @return 闭包分析结果，需要调用 ts_closure_analysis_free 释放
 * 
 * 分析规则：
 * - 识别函数体中使用但未在本地定义的变量
 * - 排除函数参数
 * - 支持简单闭包（1-10个捕获变量，单层捕获）预编译
 * - 嵌套闭包标记为不可预编译，由 MIR runtime hook 执行
 */
ts_closure_analysis_t *ts_analyze_closure(exprtk_node_t *func_body,
                                          exprtk_node_t **arg_params,
                                          size_t arg_count,
                                          exprtk_env_t *closure_env);

/**
 * @brief 释放闭包分析结果
 */
void ts_closure_analysis_free(ts_closure_analysis_t *analysis);

#ifdef __cplusplus
}
#endif

#endif // TURBO_SCRIPT_CLOSURE_ANALYSIS_H
