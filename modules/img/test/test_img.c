/**
 * @file test_img.c
 * @brief 图像处理模块单元测试
 */
#include "img.h"
#include "exprtk.h"
#include "tinytest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 320
#define TEST_HEIGHT 240

void exprtk_img_release_handles(exprtk_env_t *env);

spec("img_module") {
  describe("script handles") {
    it("should accept numeric handles and reject stale handles") {
      exprtk_env_t env;
      exprtk_value_t create_args[] = {exprtk_val_num(4), exprtk_val_num(3), exprtk_val_num(3)};
      exprtk_value_t result;
      exprtk_value_t handle_arg;
      exprtk_value_t stale_handle;

      exprtk_env_init(&env);
      exprtk_env_add_module(&env, exprtk_module_img());
      result = exprtk_call_internal("img_create", 3, create_args, &env);
      check((result.type) == (EXPRTK_VAL_INTEGER));
      check((result.data.integer) > (0));

      handle_arg = exprtk_val_num((double)result.data.integer);
      stale_handle = handle_arg;
      result = exprtk_call_internal("img_width", 1, &handle_arg, &env);
      check((result.data.integer) == (4));
      result = exprtk_call_internal("img_height", 1, &handle_arg, &env);
      check((result.data.integer) == (3));
      result = exprtk_call_internal("img_free", 1, &handle_arg, &env);
      check((result.data.integer) == (1));
      result = exprtk_call_internal("img_free", 1, &handle_arg, &env);
      check((result.data.integer) == (0));
      result = exprtk_call_internal("img_width", 1, &handle_arg, &env);
      check((result.data.integer) == (0));

      result = exprtk_call_internal("img_create", 3, create_args, &env);
      check((result.type) == (EXPRTK_VAL_INTEGER));
      check((result.data.integer) > (0));
      handle_arg = exprtk_val_num((double)result.data.integer);
      check_true(handle_arg.data.number != stale_handle.data.number);
      result = exprtk_call_internal("img_width", 1, &stale_handle, &env);
      check((result.data.integer) == (0));
      result = exprtk_call_internal("img_free", 1, &handle_arg, &env);
      check((result.data.integer) == (1));

      handle_arg = exprtk_val_num(999999999.0);
      result = exprtk_call_internal("img_width", 1, &handle_arg, &env);
      check((result.data.integer) == (0));
      exprtk_env_free(&env);
    }

    it("should isolate handles and release them with their environment") {
      exprtk_env_t owner_env;
      exprtk_env_t other_env;
      exprtk_value_t create_args[] = {exprtk_val_num(2), exprtk_val_num(2), exprtk_val_num(3)};
      exprtk_value_t result;
      exprtk_value_t handle_arg;

      exprtk_env_init(&owner_env);
      exprtk_env_init(&other_env);
      exprtk_env_add_module(&owner_env, exprtk_module_img());
      exprtk_env_add_module(&other_env, exprtk_module_img());
      result = exprtk_call_internal("img_create", 3, create_args, &owner_env);
      check((result.data.integer) > (0));
      handle_arg = exprtk_val_num((double)result.data.integer);

      result = exprtk_call_internal("img_width", 1, &handle_arg, &other_env);
      check((result.data.integer) == (0));
      exprtk_img_release_handles(&owner_env);
      result = exprtk_call_internal("img_width", 1, &handle_arg, &owner_env);
      check((result.data.integer) == (0));

      exprtk_env_free(&other_env);
      exprtk_env_free(&owner_env);
    }
  }

  describe("lifecycle") {
    it("should create and free images") {
      img_t *img = img_create(TEST_WIDTH, TEST_HEIGHT, 3);
      check_not_null(img);
      check((img->width) == (TEST_WIDTH));
      check((img->height) == (TEST_HEIGHT));
      check((img->channels) == (3));
      check_not_null(img->data);
      img_free(img);
    }

    it("should clone images") {
      img_t *src = img_create(100, 100, 3);
      check_not_null(src);

      /* 填充一些数据 */
      for (int i = 0; i < 100 * 100 * 3; i++) {
        src->data[i] = (uint8_t)(i % 256);
      }

      img_t *dst = img_clone(src);
      check_not_null(dst);
      check((dst->width) == (src->width));
      check((dst->height) == (src->height));
      check((dst->channels) == (src->channels));
      check(memcmp(dst->data, src->data, 100 * 100 * 3) == 0);

      img_free(src);
      img_free(dst);
    }
  }

  describe("pixel access") {
    it("should get and set pixels") {
      img_t *img = img_create(10, 10, 3);
      check_not_null(img);

      /* 设置像素 */
      img_set_pixel(img, 5, 5, 0, 255); /* R */
      img_set_pixel(img, 5, 5, 1, 128); /* G */
      img_set_pixel(img, 5, 5, 2, 64);  /* B */

      /* 读取像素 */
      check((img_get_pixel(img, 5, 5, 0)) == (255));
      check((img_get_pixel(img, 5, 5, 1)) == (128));
      check((img_get_pixel(img, 5, 5, 2)) == (64));

      img_free(img);
    }

    it("should handle boundary checks") {
      img_t *img = img_create(10, 10, 3);
      check_not_null(img);

      /* 边界测试 */
      check((img_get_pixel(img, -1, 5, 0)) == (-1));
      check((img_get_pixel(img, 10, 5, 0)) == (-1));
      check((img_get_pixel(img, 5, 5, 3)) == (-1));

      img_free(img);
    }
  }

  describe("geometric transforms") {
    it("should resize images") {
      img_t *src = img_create(100, 100, 3);
      check_not_null(src);

      /* 填充渐变 */
      for (int y = 0; y < 100; y++) {
        for (int x = 0; x < 100; x++) {
          size_t idx = (y * 100 + x) * 3;
          src->data[idx] = (uint8_t)x;
          src->data[idx + 1] = (uint8_t)y;
          src->data[idx + 2] = 128;
        }
      }

      img_t *resized = img_resize(src, 50, 50);
      check_not_null(resized);
      check((resized->width) == (50));
      check((resized->height) == (50));
      check((resized->channels) == (3));

      img_free(src);
      img_free(resized);
    }

    it("should crop images") {
      img_t *src = img_create(100, 100, 3);
      check_not_null(src);

      /* 裁剪中心区域 */
      img_t *cropped = img_crop(src, 25, 25, 50, 50);
      check_not_null(cropped);
      check((cropped->width) == (50));
      check((cropped->height) == (50));
      check((cropped->channels) == (3));

      img_free(src);
      img_free(cropped);
    }

    it("should reject invalid crop parameters") {
      img_t *src = img_create(100, 100, 3);
      check_not_null(src);

      /* 非法参数测试 */
      img_t *invalid = img_crop(src, -10, 0, 50, 50);
      check_null(invalid);

      invalid = img_crop(src, 0, 0, 200, 50);
      check_null(invalid);

      img_free(src);
    }

    it("should flip horizontally") {
      img_t *src = img_create(10, 10, 3);
      check_not_null(src);

      /* 设置左上角像素 */
      img_set_pixel(src, 0, 0, 0, 255);

      /* 水平翻转后应该在右上角 */
      img_t *h_flip = img_flip_horizontal(src);
      check_not_null(h_flip);
      check((img_get_pixel(h_flip, 9, 0, 0)) == (255));

      img_free(src);
      img_free(h_flip);
    }

    it("should flip vertically") {
      img_t *src = img_create(10, 10, 3);
      check_not_null(src);

      /* 设置左上角像素 */
      img_set_pixel(src, 0, 0, 0, 255);

      /* 垂直翻转后应该在左下角 */
      img_t *v_flip = img_flip_vertical(src);
      check_not_null(v_flip);
      check((img_get_pixel(v_flip, 0, 9, 0)) == (255));

      img_free(src);
      img_free(v_flip);
    }

    it("should rotate 90 degrees") {
      img_t *src = img_create(20, 10, 3);
      check_not_null(src);

      img_set_pixel(src, 0, 0, 0, 255);

      img_t *rotated = img_rotate_90(src);
      check_not_null(rotated);
      check((rotated->width) == (10));  /* 宽高互换 */
      check((rotated->height) == (20));
      check((rotated->channels) == (3));

      /* 左上角 (0,0) 旋转后到右上角 */
      check((img_get_pixel(rotated, 9, 0, 0)) == (255));

      img_free(src);
      img_free(rotated);
    }
  }

  describe("color adjustments") {
    it("should convert to grayscale") {
      img_t *src = img_create(10, 10, 3);
      check_not_null(src);

      /* 设置一个纯红色像素 */
      img_set_pixel(src, 5, 5, 0, 255);
      img_set_pixel(src, 5, 5, 1, 0);
      img_set_pixel(src, 5, 5, 2, 0);

      img_t *gray = img_to_grayscale(src);
      check_not_null(gray);
      check((gray->width) == (10));
      check((gray->height) == (10));
      check((gray->channels) == (1));

      /* 灰度值应该约为 0.299 * 255 ≈ 76 */
      int gray_value = img_get_pixel(gray, 5, 5, 0);
      check(gray_value >= 75 && gray_value <= 77);

      img_free(src);
      img_free(gray);
    }

    it("should adjust brightness") {
      img_t *src = img_create(10, 10, 3);
      check_not_null(src);

      img_set_pixel(src, 5, 5, 0, 100);

      /* 增加亮度 */
      img_t *brighter = img_adjust_brightness(src, 0.5);
      check_not_null(brighter);
      int value = img_get_pixel(brighter, 5, 5, 0);
      check(value > 100); /* 应该更亮 */
      check(value <= 255);

      /* 降低亮度 */
      img_t *darker = img_adjust_brightness(src, -0.3);
      check_not_null(darker);
      value = img_get_pixel(darker, 5, 5, 0);
      check(value < 100);
      check(value >= 0);

      img_free(src);
      img_free(brighter);
      img_free(darker);
    }

    it("should adjust contrast") {
      img_t *src = img_create(10, 10, 3);
      check_not_null(src);

      img_set_pixel(src, 5, 5, 0, 128);

      /* 增加对比度 */
      img_t *higher = img_adjust_contrast(src, 1.5);
      check_not_null(higher);

      /* 降低对比度 */
      img_t *lower = img_adjust_contrast(src, 0.5);
      check_not_null(lower);

      img_free(src);
      img_free(higher);
      img_free(lower);
    }
  }
}

