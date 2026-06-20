/**
 * @file img_plugin.c
 * @brief 图像处理插件入口
 */
#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_img(void);

TS_PLUGIN_MODULE(img, exprtk_module_img)
