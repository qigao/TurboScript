/**
 * @file img_plugin.c
 * @brief 图像处理插件入口
 */
#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_img(void);
void exprtk_img_release_handles(exprtk_env_t *env);

void *ts_plugin_open(void *env, void *scratch) {
    const exprtk_module_t *module = exprtk_module_img();
    (void)scratch;
    if (!env || !module) return NULL;
    exprtk_env_add_module((exprtk_env_t *)env, module);
    return env;
}

int32_t ts_plugin_close(void *instance) {
    exprtk_img_release_handles((exprtk_env_t *)instance);
    return 0;
}

TS_PLUGIN_PUBLISH("img")
