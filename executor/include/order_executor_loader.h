/**
 * @file order_executor_loader.h
 * @brief Utility to load order executor plugins from DLLs.
 */

#ifndef ORDER_EXECUTOR_LOADER_H
#define ORDER_EXECUTOR_LOADER_H

#include "order_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct order_executor_loader_s {
    void *handle;           /**< OS handle to the DLL */
    order_executor_t *exec; /**< The loaded executor instance */
    
    /* Internal pointers to DLL functions */
    void (*destroy_fn)(order_executor_t *self);
} order_executor_loader_t;

/**
 * @brief Load an executor from a DLL.
 * 
 * @param path         Path to the .dll or .so file.
 * @param config_json  Config string passed to the plugin's create function.
 * @return Allocated loader context, or NULL on failure.
 */
order_executor_loader_t* order_executor_load(const char *path, const char *config_json);

/**
 * @brief Unload the executor and close the DLL.
 */
void order_executor_unload(order_executor_loader_t *loader);

#ifdef __cplusplus
}
#endif

#endif /* ORDER_EXECUTOR_LOADER_H */
