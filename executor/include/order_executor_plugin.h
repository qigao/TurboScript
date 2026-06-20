/**
 * @file order_executor_plugin.h
 * @brief Binary interface for order executor plugins (DLLs).
 */

#ifndef ORDER_EXECUTOR_PLUGIN_H
#define ORDER_EXECUTOR_PLUGIN_H

#include "order_manager.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prototype for the DLL entry point.
 * Every executor plugin must implement and export a function that matches this.
 * 
 * @param config_json Implementation-specific configuration (API keys, endpoints, etc).
 * @return Allocated and initialized order_executor_t, or NULL on failure.
 */
typedef order_executor_t* (*order_executor_create_fn)(const char *config_json);

/**
 * @brief Prototype for the DLL cleanup point.
 * 
 * @param self The executor to destroy.
 */
typedef void (*order_executor_destroy_fn)(order_executor_t *self);

#ifdef __cplusplus
}
#endif

#endif /* ORDER_EXECUTOR_PLUGIN_H */
