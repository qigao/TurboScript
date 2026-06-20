/**
 * @file order_executor_loader.c
 */

#include "order_executor_loader.h"
#include "order_executor_plugin.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define RTLD_LAZY 0
static void* tt_dlopen(const char *path, int flags) { (void)flags; return (void*)LoadLibraryA(path); }
static void* tt_dlsym(void *handle, const char *name) { return (void*)GetProcAddress((HMODULE)handle, name); }
static int tt_dlclose(void *handle) { return FreeLibrary((HMODULE)handle) ? 0 : -1; }
#else
#include <dlfcn.h>
#define tt_dlopen dlopen
#define tt_dlsym dlsym
#define tt_dlclose dlclose
#endif

order_executor_loader_t* order_executor_load(const char *path, const char *config_json) {
    void *handle = tt_dlopen(path, RTLD_LAZY);
    if (!handle) return NULL;

    order_executor_create_fn create_fn = (order_executor_create_fn)tt_dlsym(handle, "turbo_executor_create");
    order_executor_destroy_fn destroy_fn = (order_executor_destroy_fn)tt_dlsym(handle, "turbo_executor_destroy");

    if (!create_fn || !destroy_fn) {
        tt_dlclose(handle);
        return NULL;
    }

    order_executor_t *exec = create_fn(config_json);
    if (!exec) {
        tt_dlclose(handle);
        return NULL;
    }

    order_executor_loader_t *loader = (order_executor_loader_t*)malloc(sizeof(order_executor_loader_t));
    if (!loader) {
        destroy_fn(exec);
        tt_dlclose(handle);
        return NULL;
    }

    loader->handle = handle;
    loader->exec = exec;
    loader->destroy_fn = destroy_fn;

    return loader;
}

void order_executor_unload(order_executor_loader_t *loader) {
    if (!loader) return;
    if (loader->exec && loader->destroy_fn) {
        loader->destroy_fn(loader->exec);
    }
    if (loader->handle) {
        tt_dlclose(loader->handle);
    }
    free(loader);
}
