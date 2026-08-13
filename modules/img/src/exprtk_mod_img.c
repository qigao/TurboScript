/**
 * @file exprtk_mod_img.c
 * @brief ExprtK 图像处理模块函数注册
 */
#include "img.h"
#include "exprtk_module.h"
#include "turbo_thread.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * 图像对象管理
 * ========================================================================= */

#ifndef IMG_HANDLE_INDEX_BITS
#define IMG_HANDLE_INDEX_BITS 10
#endif

#if IMG_HANDLE_INDEX_BITS < 1 || IMG_HANDLE_INDEX_BITS > 20
#error "IMG_HANDLE_INDEX_BITS must be between 1 and 20"
#endif

#define IMG_HANDLE_CAPACITY (UINT64_C(1) << IMG_HANDLE_INDEX_BITS)
#define IMG_HANDLE_INDEX_MASK ((uint64_t)IMG_HANDLE_CAPACITY - 1U)
#define IMG_HANDLE_MAX_GENERATION ((UINT64_C(1) << (53 - IMG_HANDLE_INDEX_BITS)) - 1U)
#define IMG_MAX_EXACT_INTEGER 9007199254740991.0
#define IMG_HANDLE_CLEANUP_BATCH_SIZE 64U

typedef struct {
    img_t *image;
    exprtk_env_t *owner;
    uint64_t generation;
    size_t active_calls;
} img_handle_slot_t;

static turbo_once_t img_handles_once = TURBO_ONCE_INIT;
static turbo_mutex_t img_handles_mutex;
static img_handle_slot_t img_handle_slots[IMG_HANDLE_CAPACITY];

static void img_handles_init(void) {
    turbo_mutex_init(&img_handles_mutex);
}

static exprtk_env_t *img_root_env(exprtk_env_t *env) {
    while (env && env->parent) env = env->parent;
    return env;
}

static int img_value_to_i64(exprtk_value_t value, int64_t *out) {
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_INTEGER) {
        *out = value.data.integer;
        return 1;
    }
    if (value.type == EXPRTK_VAL_NUMBER && isfinite(value.data.number) &&
        value.data.number >= -IMG_MAX_EXACT_INTEGER && value.data.number <= IMG_MAX_EXACT_INTEGER &&
        floor(value.data.number) == value.data.number) {
        *out = (int64_t)value.data.number;
        return 1;
    }
    return 0;
}

static int img_value_to_int(exprtk_value_t value, int *out) {
    int64_t converted;
    if (!out || !img_value_to_i64(value, &converted) || converted < INT_MIN || converted > INT_MAX)
        return 0;
    *out = (int)converted;
    return 1;
}

static int64_t img_register_handle(img_t *image, exprtk_env_t *env) {
    exprtk_env_t *owner = img_root_env(env);
    if (!image || !owner) return 0;
    turbo_once(&img_handles_once, img_handles_init);
    turbo_mutex_lock(&img_handles_mutex);
    for (size_t i = 0; i < IMG_HANDLE_CAPACITY; ++i) {
        img_handle_slot_t *slot = &img_handle_slots[i];
        if (slot->image || slot->active_calls != 0) continue;
        slot->generation++;
        if (slot->generation == 0 || slot->generation > IMG_HANDLE_MAX_GENERATION)
            slot->generation = 1;
        slot->image = image;
        slot->owner = owner;
        int64_t handle = (int64_t)((slot->generation << IMG_HANDLE_INDEX_BITS) | i);
        turbo_mutex_unlock(&img_handles_mutex);
        return handle;
    }
    turbo_mutex_unlock(&img_handles_mutex);
    return 0;
}

static img_t *img_acquire_handle(exprtk_value_t value, exprtk_env_t *env, int64_t *handle_out) {
    int64_t handle;
    uint64_t encoded;
    size_t index;
    uint64_t generation;
    img_t *image = NULL;

    exprtk_env_t *owner = img_root_env(env);
    if (!owner || !img_value_to_i64(value, &handle) || handle <= 0) return NULL;
    encoded = (uint64_t)handle;
    index = (size_t)(encoded & IMG_HANDLE_INDEX_MASK);
    generation = encoded >> IMG_HANDLE_INDEX_BITS;
    if (generation == 0 || generation > IMG_HANDLE_MAX_GENERATION) return NULL;

    turbo_once(&img_handles_once, img_handles_init);
    turbo_mutex_lock(&img_handles_mutex);
    if (img_handle_slots[index].image && img_handle_slots[index].owner == owner &&
        img_handle_slots[index].generation == generation) {
        img_handle_slots[index].active_calls++;
        image = img_handle_slots[index].image;
        if (handle_out) *handle_out = handle;
    }
    turbo_mutex_unlock(&img_handles_mutex);
    return image;
}

static void img_release_handle(int64_t handle) {
    size_t index = (size_t)((uint64_t)handle & IMG_HANDLE_INDEX_MASK);
    turbo_mutex_lock(&img_handles_mutex);
    if (img_handle_slots[index].active_calls > 0) img_handle_slots[index].active_calls--;
    turbo_mutex_unlock(&img_handles_mutex);
}

static int img_destroy_handle(exprtk_value_t value, exprtk_env_t *env) {
    int64_t handle;
    uint64_t encoded;
    size_t index;
    uint64_t generation;
    img_t *image;

    exprtk_env_t *owner = img_root_env(env);
    if (!owner || !img_value_to_i64(value, &handle) || handle <= 0) return 0;
    encoded = (uint64_t)handle;
    index = (size_t)(encoded & IMG_HANDLE_INDEX_MASK);
    generation = encoded >> IMG_HANDLE_INDEX_BITS;
    turbo_once(&img_handles_once, img_handles_init);
    turbo_mutex_lock(&img_handles_mutex);
    if (!img_handle_slots[index].image || img_handle_slots[index].owner != owner ||
        img_handle_slots[index].generation != generation || img_handle_slots[index].active_calls != 0) {
        turbo_mutex_unlock(&img_handles_mutex);
        return 0;
    }
    image = img_handle_slots[index].image;
    img_handle_slots[index].image = NULL;
    img_handle_slots[index].owner = NULL;
    turbo_mutex_unlock(&img_handles_mutex);
    img_free(image);
    return 1;
}

static exprtk_value_t img_result_handle(img_t *image, exprtk_env_t *env) {
    int64_t handle = img_register_handle(image, env);
    if (!handle && image) img_free(image);
    return exprtk_val_int(handle);
}

void exprtk_img_release_handles(exprtk_env_t *env) {
    exprtk_env_t *owner = env;
    img_t *images[IMG_HANDLE_CLEANUP_BATCH_SIZE];
    size_t next_slot = 0;

    if (!owner) return;
    turbo_once(&img_handles_once, img_handles_init);
    while (next_slot < IMG_HANDLE_CAPACITY) {
        size_t image_count = 0;
        turbo_mutex_lock(&img_handles_mutex);
        while (next_slot < IMG_HANDLE_CAPACITY && image_count < IMG_HANDLE_CLEANUP_BATCH_SIZE) {
            img_handle_slot_t *slot = &img_handle_slots[next_slot++];
            if (!slot->image || slot->owner != owner || slot->active_calls != 0) continue;
            images[image_count++] = slot->image;
            slot->image = NULL;
            slot->owner = NULL;
        }
        turbo_mutex_unlock(&img_handles_mutex);

        for (size_t i = 0; i < image_count; ++i) img_free(images[i]);
    }
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
    (void)arena;

    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
        return exprtk_val_int(0);
    }

    int desired_channels = 0;
    if (argc >= 2 && !img_value_to_int(args[1], &desired_channels)) return exprtk_val_int(0);

    const char *path = args[0].data.string.data;
    img_t *img = img_load(path, desired_channels);

    return img_result_handle(img, env);
}

/**
 * img_save(handle: int, path: string, format: int = 0, quality: int = 90) -> int
 * format: 0=PNG, 1=JPEG, 2=BMP, 3=TGA
 */
static exprtk_value_t fn_img_save(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)arena;

    if (argc < 2) return exprtk_val_int(0);
    if (args[1].type != EXPRTK_VAL_STRING) return exprtk_val_int(0);

    int64_t handle;
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);

    const char *path = args[1].data.string.data;

    img_format_t format = IMG_FORMAT_PNG;
    int format_value = IMG_FORMAT_PNG;
    if (argc >= 3 && !img_value_to_int(args[2], &format_value)) {
        img_release_handle(handle);
        return exprtk_val_int(0);
    }
    format = (img_format_t)format_value;

    int quality = 90;
    if (argc >= 4 && !img_value_to_int(args[3], &quality)) {
        img_release_handle(handle);
        return exprtk_val_int(0);
    }

    int result = img_save(img, path, format, quality);
    img_release_handle(handle);
    return exprtk_val_int(result);
}

/**
 * img_free(handle: int) -> void
 */
static exprtk_value_t fn_img_free(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)arena;

    if (argc < 1) return exprtk_val_int(0);
    return exprtk_val_int(img_destroy_handle(args[0], env));
}

/**
 * img_create(width: int, height: int, channels: int) -> int
 */
static exprtk_value_t fn_img_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)arena;

    if (argc < 3) return exprtk_val_int(0);
    int width;
    int height;
    int channels;
    if (!img_value_to_int(args[0], &width) || !img_value_to_int(args[1], &height) ||
        !img_value_to_int(args[2], &channels))
        return exprtk_val_int(0);

    img_t *img = img_create(width, height, channels);
    return img_result_handle(img, env);
}

/**
 * img_width(handle: int) -> int
 */
static exprtk_value_t fn_img_width(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
    (void)arena;

    int64_t handle;
    if (argc < 1) return exprtk_val_int(0);
    img_t *img = img_acquire_handle(args[0], env, &handle);
    int width = img ? img->width : 0;
    if (img) img_release_handle(handle);
    return exprtk_val_int(width);
}

/**
 * img_height(handle: int) -> int
 */
static exprtk_value_t fn_img_height(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)arena;

    int64_t handle;
    if (argc < 1) return exprtk_val_int(0);
    img_t *img = img_acquire_handle(args[0], env, &handle);
    int height = img ? img->height : 0;
    if (img) img_release_handle(handle);
    return exprtk_val_int(height);
}

/**
 * img_channels(handle: int) -> int
 */
static exprtk_value_t fn_img_channels(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
    (void)arena;

    int64_t handle;
    if (argc < 1) return exprtk_val_int(0);
    img_t *img = img_acquire_handle(args[0], env, &handle);
    int channels = img ? img->channels : 0;
    if (img) img_release_handle(handle);
    return exprtk_val_int(channels);
}

/**
 * img_resize(handle: int, width: int, height: int) -> int
 */
static exprtk_value_t fn_img_resize(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
    (void)arena;

    if (argc < 3) return exprtk_val_int(0);
    int64_t handle;
    int width;
    int height;
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);
    if (!img_value_to_int(args[1], &width) || !img_value_to_int(args[2], &height)) {
        img_release_handle(handle);
        return exprtk_val_int(0);
    }

    img_t *result = img_resize(img, width, height);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_crop(handle: int, x: int, y: int, width: int, height: int) -> int
 */
static exprtk_value_t fn_img_crop(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
    (void)arena;

    if (argc < 5) return exprtk_val_int(0);
    int64_t handle;
    int x;
    int y;
    int width;
    int height;
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);
    if (!img_value_to_int(args[1], &x) || !img_value_to_int(args[2], &y) ||
        !img_value_to_int(args[3], &width) || !img_value_to_int(args[4], &height)) {
        img_release_handle(handle);
        return exprtk_val_int(0);
    }

    img_t *result = img_crop(img, x, y, width, height);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_to_grayscale(handle: int) -> int
 */
static exprtk_value_t fn_img_to_grayscale(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           mem_pool_t *arena) {
    (void)arena;

    int64_t handle;
    if (argc < 1) return exprtk_val_int(0);
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_to_grayscale(img);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_flip_horizontal(handle: int) -> int
 */
static exprtk_value_t fn_img_flip_horizontal(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                              mem_pool_t *arena) {
    (void)arena;

    int64_t handle;
    if (argc < 1) return exprtk_val_int(0);
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_flip_horizontal(img);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_flip_vertical(handle: int) -> int
 */
static exprtk_value_t fn_img_flip_vertical(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                            mem_pool_t *arena) {
    (void)arena;

    int64_t handle;
    if (argc < 1) return exprtk_val_int(0);
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_flip_vertical(img);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_rotate_90(handle: int) -> int
 */
static exprtk_value_t fn_img_rotate_90(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)arena;

    int64_t handle;
    if (argc < 1) return exprtk_val_int(0);
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);

    img_t *result = img_rotate_90(img);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_adjust_brightness(handle: int, factor: double) -> int
 */
static exprtk_value_t fn_img_adjust_brightness(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                                mem_pool_t *arena) {
    (void)arena;

    if (argc < 2) return exprtk_val_int(0);
    int64_t handle;
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);

    double factor = 0.0;
    if (args[1].type == EXPRTK_VAL_NUMBER) {
        factor = args[1].data.number;
    } else if (args[1].type == EXPRTK_VAL_INTEGER) {
        factor = (double)args[1].data.integer;
    } else {
        img_release_handle(handle);
        return exprtk_val_int(0);
    }

    img_t *result = img_adjust_brightness(img, factor);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_adjust_contrast(handle: int, factor: double) -> int
 */
static exprtk_value_t fn_img_adjust_contrast(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                              mem_pool_t *arena) {
    (void)arena;

    if (argc < 2) return exprtk_val_int(0);
    int64_t handle;
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);

    double factor = 1.0;
    if (args[1].type == EXPRTK_VAL_NUMBER) {
        factor = args[1].data.number;
    } else if (args[1].type == EXPRTK_VAL_INTEGER) {
        factor = (double)args[1].data.integer;
    } else {
        img_release_handle(handle);
        return exprtk_val_int(0);
    }

    img_t *result = img_adjust_contrast(img, factor);
    img_release_handle(handle);
    return img_result_handle(result, env);
}

/**
 * img_get_pixel(handle: int, x: int, y: int, channel: int) -> int
 */
static exprtk_value_t fn_img_get_pixel(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)arena;

    if (argc < 4) return exprtk_val_int(-1);
    int64_t handle;
    int x;
    int y;
    int channel;
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(-1);
    if (!img_value_to_int(args[1], &x) || !img_value_to_int(args[2], &y) ||
        !img_value_to_int(args[3], &channel)) {
        img_release_handle(handle);
        return exprtk_val_int(-1);
    }

    int value = img_get_pixel(img, x, y, channel);
    img_release_handle(handle);
    return exprtk_val_int(value);
}

/**
 * img_set_pixel(handle: int, x: int, y: int, channel: int, value: int) -> void
 */
static exprtk_value_t fn_img_set_pixel(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
    (void)arena;

    if (argc < 5) return exprtk_val_int(0);
    int64_t handle;
    int x;
    int y;
    int channel;
    int value;
    img_t *img = img_acquire_handle(args[0], env, &handle);
    if (!img) return exprtk_val_int(0);
    if (!img_value_to_int(args[1], &x) || !img_value_to_int(args[2], &y) ||
        !img_value_to_int(args[3], &channel) || !img_value_to_int(args[4], &value) ||
        value < 0 || value > UINT8_MAX) {
        img_release_handle(handle);
        return exprtk_val_int(0);
    }

    img_set_pixel(img, x, y, channel, (uint8_t)value);
    img_release_handle(handle);
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
