/**
 * @file img_plugin.c
 * @brief Image module plugin entry through the canonical Salts Plugin ABI.
 */
#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_img(void);
void exprtk_img_release_handles(exprtk_env_t *env);

TS_PLUGIN_MODULE_WITH_UNLOAD(img, exprtk_module_img, exprtk_img_release_handles)
