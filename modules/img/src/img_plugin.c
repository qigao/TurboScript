/**
 * @file img_plugin.c
 * @brief 图像处理插件入口
 */
#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_img(void);
void exprtk_img_release_handles(exprtk_env_t *env);

static void *img_plugin_load(void *env, void *scratch) {
    const exprtk_module_t *module = exprtk_module_img();
    (void)scratch;
    if (!env || !module) return NULL;
    exprtk_env_add_module((exprtk_env_t *)env, module);
    return env;
}

static void img_plugin_unload(void *instance) {
    exprtk_img_release_handles((exprtk_env_t *)instance);
}

static const ts_plugin_t img_plugin = {
    .name = "img",
    .version = 1,
    .load = img_plugin_load,
    .unload = img_plugin_unload,
};

TS_PLUGIN_C_API const ts_plugin_t *ts_api_create(void) {
    return &img_plugin;
}
