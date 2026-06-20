/**
 * @file img.h
 * @brief Image processing module using stb_image/stb_image_write
 *
 * 提供图像加载、保存、基本变换功能
 * 支持 JPEG、PNG、BMP、TGA 等常见格式
 */
#ifndef IMG_H
#define IMG_H

#include "exprtk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 图像数据结构
 */
typedef struct {
    uint8_t *data;      /**< 像素数据 (R,G,B,A 顺序) */
    int width;          /**< 宽度 */
    int height;         /**< 高度 */
    int channels;       /**< 通道数 (1=灰度, 3=RGB, 4=RGBA) */
    int owns_data;      /**< 是否拥有数据所有权 */
} img_t;

/**
 * @brief 图像格式枚举
 */
typedef enum {
    IMG_FORMAT_PNG = 0,
    IMG_FORMAT_JPEG,
    IMG_FORMAT_BMP,
    IMG_FORMAT_TGA
} img_format_t;

/* =========================================================================
 * Core API
 * ========================================================================= */

/**
 * @brief 从文件加载图像
 * @param path 文件路径
 * @param desired_channels 期望通道数 (0=自动检测)
 * @return 图像对象指针，失败返回 NULL
 */
img_t *img_load(const char *path, int desired_channels);

/**
 * @brief 从内存缓冲区加载图像
 * @param buffer 图像数据缓冲区
 * @param size 缓冲区大小
 * @param desired_channels 期望通道数
 * @return 图像对象指针，失败返回 NULL
 */
img_t *img_load_from_memory(const uint8_t *buffer, size_t size, int desired_channels);

/**
 * @brief 保存图像到文件
 * @param img 图像对象
 * @param path 输出路径
 * @param format 输出格式
 * @param quality JPEG 质量 (1-100)，其他格式忽略
 * @return 成功返回 1，失败返回 0
 */
int img_save(const img_t *img, const char *path, img_format_t format, int quality);

/**
 * @brief 创建空白图像
 * @param width 宽度
 * @param height 高度
 * @param channels 通道数
 * @return 图像对象指针，失败返回 NULL
 */
img_t *img_create(int width, int height, int channels);

/**
 * @brief 复制图像
 */
img_t *img_clone(const img_t *src);

/**
 * @brief 释放图像
 */
void img_free(img_t *img);

/* =========================================================================
 * Image Operations
 * ========================================================================= */

/**
 * @brief 调整图像大小（最近邻插值）
 */
img_t *img_resize(const img_t *src, int new_width, int new_height);

/**
 * @brief 裁剪图像
 */
img_t *img_crop(const img_t *src, int x, int y, int width, int height);

/**
 * @brief 转换为灰度图
 */
img_t *img_to_grayscale(const img_t *src);

/**
 * @brief 水平翻转
 */
img_t *img_flip_horizontal(const img_t *src);

/**
 * @brief 垂直翻转
 */
img_t *img_flip_vertical(const img_t *src);

/**
 * @brief 旋转 90 度（顺时针）
 */
img_t *img_rotate_90(const img_t *src);

/**
 * @brief 调整亮度 (-1.0 到 1.0)
 */
img_t *img_adjust_brightness(const img_t *src, double factor);

/**
 * @brief 调整对比度 (0.0 到 2.0, 1.0 为原始)
 */
img_t *img_adjust_contrast(const img_t *src, double factor);

/**
 * @brief 获取像素值
 * @param img 图像对象
 * @param x X 坐标
 * @param y Y 坐标
 * @param channel 通道索引 (0-3)
 * @return 像素值 (0-255)
 */
int img_get_pixel(const img_t *img, int x, int y, int channel);

/**
 * @brief 设置像素值
 */
void img_set_pixel(img_t *img, int x, int y, int channel, uint8_t value);

/* =========================================================================
 * ExprtK Module
 * ========================================================================= */

/**
 * @brief ExprtK 模块注册函数
 */
const exprtk_module_t *exprtk_module_img(void);

#ifdef __cplusplus
}
#endif

#endif /* IMG_H */
