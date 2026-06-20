/**
 * @file img_core.c
 * @brief 图像核心功能实现
 */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include "img.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Core API
 * ========================================================================= */

img_t *img_load(const char *path, int desired_channels) {
    if (!path) return NULL;

    img_t *img = (img_t *)calloc(1, sizeof(img_t));
    if (!img) return NULL;

    img->data = stbi_load(path, &img->width, &img->height, &img->channels, desired_channels);
    if (!img->data) {
        free(img);
        return NULL;
    }

    if (desired_channels > 0) {
        img->channels = desired_channels;
    }
    img->owns_data = 1;

    return img;
}

img_t *img_load_from_memory(const uint8_t *buffer, size_t size, int desired_channels) {
    if (!buffer || size == 0) return NULL;

    img_t *img = (img_t *)calloc(1, sizeof(img_t));
    if (!img) return NULL;

    img->data = stbi_load_from_memory(buffer, (int)size, &img->width, &img->height,
                                      &img->channels, desired_channels);
    if (!img->data) {
        free(img);
        return NULL;
    }

    if (desired_channels > 0) {
        img->channels = desired_channels;
    }
    img->owns_data = 1;

    return img;
}

int img_save(const img_t *img, const char *path, img_format_t format, int quality) {
    if (!img || !img->data || !path) return 0;

    int result = 0;
    switch (format) {
    case IMG_FORMAT_PNG:
        result = stbi_write_png(path, img->width, img->height, img->channels, img->data,
                                img->width * img->channels);
        break;
    case IMG_FORMAT_JPEG:
        result = stbi_write_jpg(path, img->width, img->height, img->channels, img->data,
                                quality > 0 ? quality : 90);
        break;
    case IMG_FORMAT_BMP:
        result = stbi_write_bmp(path, img->width, img->height, img->channels, img->data);
        break;
    case IMG_FORMAT_TGA:
        result = stbi_write_tga(path, img->width, img->height, img->channels, img->data);
        break;
    default:
        return 0;
    }

    return result;
}

img_t *img_create(int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4) return NULL;

    img_t *img = (img_t *)calloc(1, sizeof(img_t));
    if (!img) return NULL;

    size_t data_size = (size_t)width * height * channels;
    img->data = (uint8_t *)calloc(data_size, sizeof(uint8_t));
    if (!img->data) {
        free(img);
        return NULL;
    }

    img->width = width;
    img->height = height;
    img->channels = channels;
    img->owns_data = 1;

    return img;
}

img_t *img_clone(const img_t *src) {
    if (!src || !src->data) return NULL;

    img_t *dst = img_create(src->width, src->height, src->channels);
    if (!dst) return NULL;

    size_t data_size = (size_t)src->width * src->height * src->channels;
    memcpy(dst->data, src->data, data_size);

    return dst;
}

void img_free(img_t *img) {
    if (!img) return;
    if (img->owns_data && img->data) {
        stbi_image_free(img->data);
    }
    free(img);
}

/* =========================================================================
 * Image Operations
 * ========================================================================= */

img_t *img_resize(const img_t *src, int new_width, int new_height) {
    if (!src || !src->data || new_width <= 0 || new_height <= 0) return NULL;

    img_t *dst = img_create(new_width, new_height, src->channels);
    if (!dst) return NULL;

    /* 最近邻插值 */
    for (int y = 0; y < new_height; y++) {
        for (int x = 0; x < new_width; x++) {
            int src_x = (int)((double)x * src->width / new_width);
            int src_y = (int)((double)y * src->height / new_height);

            if (src_x >= src->width) src_x = src->width - 1;
            if (src_y >= src->height) src_y = src->height - 1;

            size_t src_idx = (size_t)(src_y * src->width + src_x) * src->channels;
            size_t dst_idx = (size_t)(y * new_width + x) * dst->channels;

            for (int c = 0; c < src->channels; c++) {
                dst->data[dst_idx + c] = src->data[src_idx + c];
            }
        }
    }

    return dst;
}

img_t *img_crop(const img_t *src, int x, int y, int width, int height) {
    if (!src || !src->data) return NULL;
    if (x < 0 || y < 0 || width <= 0 || height <= 0) return NULL;
    if (x + width > src->width || y + height > src->height) return NULL;

    img_t *dst = img_create(width, height, src->channels);
    if (!dst) return NULL;

    for (int dy = 0; dy < height; dy++) {
        size_t src_idx = (size_t)((y + dy) * src->width + x) * src->channels;
        size_t dst_idx = (size_t)(dy * width) * dst->channels;
        memcpy(&dst->data[dst_idx], &src->data[src_idx], (size_t)width * src->channels);
    }

    return dst;
}

img_t *img_to_grayscale(const img_t *src) {
    if (!src || !src->data) return NULL;

    img_t *dst = img_create(src->width, src->height, 1);
    if (!dst) return NULL;

    for (int i = 0; i < src->width * src->height; i++) {
        size_t src_idx = (size_t)i * src->channels;
        if (src->channels >= 3) {
            /* 标准灰度转换公式 */
            double gray = 0.299 * src->data[src_idx] + 0.587 * src->data[src_idx + 1] +
                          0.114 * src->data[src_idx + 2];
            dst->data[i] = (uint8_t)gray;
        } else {
            dst->data[i] = src->data[src_idx];
        }
    }

    return dst;
}

img_t *img_flip_horizontal(const img_t *src) {
    if (!src || !src->data) return NULL;

    img_t *dst = img_create(src->width, src->height, src->channels);
    if (!dst) return NULL;

    for (int y = 0; y < src->height; y++) {
        for (int x = 0; x < src->width; x++) {
            size_t src_idx = (size_t)(y * src->width + x) * src->channels;
            size_t dst_idx = (size_t)(y * dst->width + (dst->width - 1 - x)) * dst->channels;

            for (int c = 0; c < src->channels; c++) {
                dst->data[dst_idx + c] = src->data[src_idx + c];
            }
        }
    }

    return dst;
}

img_t *img_flip_vertical(const img_t *src) {
    if (!src || !src->data) return NULL;

    img_t *dst = img_create(src->width, src->height, src->channels);
    if (!dst) return NULL;

    for (int y = 0; y < src->height; y++) {
        size_t src_idx = (size_t)(y * src->width) * src->channels;
        size_t dst_idx = (size_t)((dst->height - 1 - y) * dst->width) * dst->channels;
        memcpy(&dst->data[dst_idx], &src->data[src_idx], (size_t)src->width * src->channels);
    }

    return dst;
}

img_t *img_rotate_90(const img_t *src) {
    if (!src || !src->data) return NULL;

    /* 旋转后宽高互换 */
    img_t *dst = img_create(src->height, src->width, src->channels);
    if (!dst) return NULL;

    for (int y = 0; y < src->height; y++) {
        for (int x = 0; x < src->width; x++) {
            size_t src_idx = (size_t)(y * src->width + x) * src->channels;
            size_t dst_idx = (size_t)(x * dst->width + (dst->width - 1 - y)) * dst->channels;

            for (int c = 0; c < src->channels; c++) {
                dst->data[dst_idx + c] = src->data[src_idx + c];
            }
        }
    }

    return dst;
}

img_t *img_adjust_brightness(const img_t *src, double factor) {
    if (!src || !src->data) return NULL;

    img_t *dst = img_clone(src);
    if (!dst) return NULL;

    int adjustment = (int)(factor * 255.0);
    size_t total_pixels = (size_t)src->width * src->height * src->channels;

    for (size_t i = 0; i < total_pixels; i++) {
        int value = (int)dst->data[i] + adjustment;
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        dst->data[i] = (uint8_t)value;
    }

    return dst;
}

img_t *img_adjust_contrast(const img_t *src, double factor) {
    if (!src || !src->data) return NULL;

    img_t *dst = img_clone(src);
    if (!dst) return NULL;

    size_t total_pixels = (size_t)src->width * src->height * src->channels;

    for (size_t i = 0; i < total_pixels; i++) {
        double value = (dst->data[i] - 128.0) * factor + 128.0;
        if (value < 0.0) value = 0.0;
        if (value > 255.0) value = 255.0;
        dst->data[i] = (uint8_t)value;
    }

    return dst;
}

int img_get_pixel(const img_t *img, int x, int y, int channel) {
    if (!img || !img->data) return -1;
    if (x < 0 || x >= img->width || y < 0 || y >= img->height) return -1;
    if (channel < 0 || channel >= img->channels) return -1;

    size_t idx = (size_t)(y * img->width + x) * img->channels + channel;
    return (int)img->data[idx];
}

void img_set_pixel(img_t *img, int x, int y, int channel, uint8_t value) {
    if (!img || !img->data) return;
    if (x < 0 || x >= img->width || y < 0 || y >= img->height) return;
    if (channel < 0 || channel >= img->channels) return;

    size_t idx = (size_t)(y * img->width + x) * img->channels + channel;
    img->data[idx] = value;
}
