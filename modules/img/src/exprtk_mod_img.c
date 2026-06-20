/**
 * @file exprtk_mod_img.c
 * @brief ExprtK 图像处理模块函数注册
 */
#include "img.h"
#include "exprtk_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * 图像对象管理（使用 MAP 存储）
 * ========================================================================= */

static img_t *img_from_handle(int64_t handle) {
    if (handle <= 0) return NULL;
    return (img_t *)(uintptr_t)handle;
}

static int64_t img_to_handle(img_t *img) {
    return (int64_t)(uintptr_t)img;
}

/* =========================================================================
 * ExprtK 函数包装器
 * ========================================================================= */

/**
 * img_load(path: string, channels: int = 0) -> int
 * 返回图像句柄
 */
static exprtk_value_t fn_img_load(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
        return exprtk_val_int(0);
    }

    int desired_channels = 0;
    if (argc >= 2 && args[1].type == EXPRTK_VAL_INTEGER) {
        desired_channels = (int)args[1].data.integer;
    }

    const char *path = args[0].data.string.data;
    img_t *img = img_load(path, desired_channels);

    return exprtk_val_int(img_to_handle(img));
}

/**
 * img_save(handle: int, path: string, format: int = 0, quality: int = 90) -> int
 * format: 0=PNG, 1=JPEG, 2=BMP, 3=TGA
 */
static exprtk_value_t fn_img_save(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 2) return exprtk_val_int(0);
    if (args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);
    if (args[1].type != EXPRTK_VAL_STRING) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    const char *path = args[1].data.string.data;

    img_format_t format = IMG_FORMAT_PNG;
    if (argc >= 3 && args[2].type == EXPRTK_VAL_INTEGER) {
        format = (img_format_t)args[2].data.integer;
    }

    int quality = 90;
    if (argc >= 4 && args[3].type == EXPRTK_VAL_INTEGER) {
        quality = (int)args[3].data.integer;
    }

    int result = img_save(img, path, format, quality);
    return exprtk_val_int(result);
}

/**
 * img_free(handle: int) -> void
 */
static exprtk_value_t fn_img_free(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) {
        return exprtk_val_int(0);
    }

    img_t *img = img_from_handle(args[0].data.integer);
    if (img) {
        img_free(img);
    }

    return exprtk_val_int(1);
}

/**
 * img_create(width: int, height: int, channels: int) -> int
 */
static exprtk_value_t fn_img_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 3) return exprtk_val_int(0);
    if (args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);
    if (args[1].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);
    if (args[2].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    int width = (int)args[0].data.integer;
    int height = (int)args[1].data.integer;
    int channels = (int)args[2].data.integer;

    img_t *img = img_create(width, height, channels);
    return exprtk_val_int(img_to_handle(img));
}

/**
 * img_width(handle: int) -> int
 */
static exprtk_value_t fn_img_width(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    return exprtk_val_int(img ? img->width : 0);
}

/**
 * img_height(handle: int) -> int
 */
static exprtk_value_t fn_img_height(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    return exprtk_val_int(img ? img->height : 0);
}

/**
 * img_channels(handle: int) -> int
 */
static exprtk_value_t fn_img_channels(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    return exprtk_val_int(img ? img->channels : 0);
}

/**
 * img_resize(handle: int, width: int, height: int) -> int
 */
static exprtk_value_t fn_img_resize(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 3) return exprtk_val_int(0);
    if (args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);
    if (args[1].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);
    if (args[2].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    int width = (int)args[1].data.integer;
    int height = (int)args[2].data.integer;

    img_t *result = img_resize(img, width, height);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_crop(handle: int, x: int, y: int, width: int, height: int) -> int
 */
static exprtk_value_t fn_img_crop(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 5) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    int x = (int)args[1].data.integer;
    int y = (int)args[2].data.integer;
    int width = (int)args[3].data.integer;
    int height = (int)args[4].data.integer;

    img_t *result = img_crop(img, x, y, width, height);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_to_grayscale(handle: int) -> int
 */
static exprtk_value_t fn_img_to_grayscale(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_to_grayscale(img);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_flip_horizontal(handle: int) -> int
 */
static exprtk_value_t fn_img_flip_horizontal(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                              mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_flip_horizontal(img);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_flip_vertical(handle: int) -> int
 */
static exprtk_value_t fn_img_flip_vertical(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                            mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_flip_vertical(img);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_rotate_90(handle: int) -> int
 */
static exprtk_value_t fn_img_rotate_90(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_rotate_90(img);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_adjust_brightness(handle: int, factor: double) -> int
 */
static exprtk_value_t fn_img_adjust_brightness(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                                mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 2) return exprtk_val_int(0);
    if (args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    double factor = 0.0;
    if (args[1].type == EXPRTK_VAL_NUMBER) {
        factor = args[1].data.number;
    } else if (args[1].type == EXPRTK_VAL_INTEGER) {
        factor = (double)args[1].data.integer;
    }

    img_t *result = img_adjust_brightness(img, factor);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_adjust_contrast(handle: int, factor: double) -> int
 */
static exprtk_value_t fn_img_adjust_contrast(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                              mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 2) return exprtk_val_int(0);
    if (args[0].type != EXPRTK_VAL_INTEGER) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    double factor = 1.0;
    if (args[1].type == EXPRTK_VAL_NUMBER) {
        factor = args[1].data.number;
    } else if (args[1].type == EXPRTK_VAL_INTEGER) {
        factor = (double)args[1].data.integer;
    }

    img_t *result = img_adjust_contrast(img, factor);
    return exprtk_val_int(img_to_handle(result));
}

/**
 * img_get_pixel(handle: int, x: int, y: int, channel: int) -> int
 */
static exprtk_value_t fn_img_get_pixel(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 4) return exprtk_val_int(-1);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(-1);

    int x = (int)args[1].data.integer;
    int y = (int)args[2].data.integer;
    int channel = (int)args[3].data.integer;

    int value = img_get_pixel(img, x, y, channel);
    return exprtk_val_int(value);
}

/**
 * img_set_pixel(handle: int, x: int, y: int, channel: int, value: int) -> void
 */
static exprtk_value_t fn_img_set_pixel(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)env;
    (void)arena;

    if (argc < 5) return exprtk_val_int(0);

    img_t *img = img_from_handle(args[0].data.integer);
    if (!img) return exprtk_val_int(0);

    int x = (int)args[1].data.integer;
    int y = (int)args[2].data.integer;
    int channel = (int)args[3].data.integer;
    uint8_t value = (uint8_t)args[4].data.integer;

    img_set_pixel(img, x, y, channel, value);
    return exprtk_val_int(1);
}

/* =========================================================================
 * Module Registration
 * ========================================================================= */

static const exprtk_func_entry_t img_functions[] = {
    {"img_load", fn_img_load},
    {"img_save", fn_img_save},
    {"img_free", fn_img_free},
    {"img_create", fn_img_create},
    {"img_width", fn_img_width},
    {"img_height", fn_img_height},
    {"img_channels", fn_img_channels},
    {"img_resize", fn_img_resize},
    {"img_crop", fn_img_crop},
    {"img_to_grayscale", fn_img_to_grayscale},
    {"img_flip_horizontal", fn_img_flip_horizontal},
    {"img_flip_vertical", fn_img_flip_vertical},
    {"img_rotate_90", fn_img_rotate_90},
    {"img_adjust_brightness", fn_img_adjust_brightness},
    {"img_adjust_contrast", fn_img_adjust_contrast},
    {"img_get_pixel", fn_img_get_pixel},
    {"img_set_pixel", fn_img_set_pixel},
};

static const exprtk_module_t img_module = {
    .module_name = "img",
    .entries = img_functions,
    .count = sizeof(img_functions) / sizeof(img_functions[0]),
};

const exprtk_module_t *exprtk_module_img(void) { return &img_module; }
