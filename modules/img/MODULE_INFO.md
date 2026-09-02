# IMG 模块 - 模块信息

## 基本信息

- **模块名称**: img
- **版本**: 1.0.0
- **类型**: TurboScript 插件模块
- **语言**: C11
- **依赖库**: stb_image, stb_image_write

## 模块结构

```
modules/img/
├── CMakeLists.txt           # 构建配置
├── README.md                # 详细文档
├── USAGE_GUIDE.md           # 快速上手指南
├── MODULE_INFO.md           # 本文件
├── include/
│   └── img.h                # 公共 API 头文件
├── src/
│   ├── img_core.c           # 核心图像处理实现
│   ├── exprtk_mod_img.c     # ExprtK 函数包装
│   └── img_plugin.c         # 插件入口
└── test/
    ├── CMakeLists.txt       # 测试构建配置
    ├── test_img.c           # C 单元测试
    └── example_usage.tbs     # TurboScript 使用示例
```

## 构建产物

### 静态库 (用于测试)
- **名称**: `libimg.a` / `img.lib`
- **用途**: 直接链接到单元测试
- **别名**: `TurboScript::IMG`

### 动态插件 (运行时加载)
- **名称**: `img.dll` / `img.so`
- **用途**: TurboScript 运行时通过 `import("img")` 加载

## 依赖关系

### 编译时依赖
- `exprtk` (TurboScript 核心)
- `Rocida::Core` (工具库)
- `stb` (通过 vcpkg)

### 运行时依赖
- 无额外运行时依赖

## API 概览

### C API (img.h)

#### 生命周期管理
```c
img_t *img_load(const char *path, int desired_channels);
img_t *img_create(int width, int height, int channels);
img_t *img_clone(const img_t *src);
void img_free(img_t *img);
int img_save(const img_t *img, const char *path, img_format_t format, int quality);
```

#### 几何变换
```c
img_t *img_resize(const img_t *src, int new_width, int new_height);
img_t *img_crop(const img_t *src, int x, int y, int width, int height);
img_t *img_flip_horizontal(const img_t *src);
img_t *img_flip_vertical(const img_t *src);
img_t *img_rotate_90(const img_t *src);
```

#### 颜色处理
```c
img_t *img_to_grayscale(const img_t *src);
img_t *img_adjust_brightness(const img_t *src, double factor);
img_t *img_adjust_contrast(const img_t *src, double factor);
```

#### 像素访问
```c
int img_get_pixel(const img_t *img, int x, int y, int channel);
void img_set_pixel(img_t *img, int x, int y, int channel, uint8_t value);
```

### TurboScript API

所有函数通过 `exprtk_module_img()` 注册，可在脚本中直接调用：

```javascript
import("img");

// 函数列表（17 个）
img_load(path, channels)
img_save(handle, path, format, quality)
img_free(handle)
img_create(width, height, channels)
img_width(handle)
img_height(handle)
img_channels(handle)
img_resize(handle, width, height)
img_crop(handle, x, y, width, height)
img_to_grayscale(handle)
img_flip_horizontal(handle)
img_flip_vertical(handle)
img_rotate_90(handle)
img_adjust_brightness(handle, factor)
img_adjust_contrast(handle, factor)
img_get_pixel(handle, x, y, channel)
img_set_pixel(handle, x, y, channel, value)
```

## 技术特性

### 图像格式支持

#### 读取 (stb_image)
- JPEG (baseline & progressive)
- PNG (1/2/4/8/16-bit per channel)
- TGA
- BMP (non-RLE)
- PSD (composited view)
- GIF (non-animated)
- HDR (radiance rgbE)
- PIC (Softimage)
- PNM (PPM/PGM)

#### 写入 (stb_image_write)
- PNG (无损)
- JPEG (有损, 可调质量)
- BMP (无压缩)
- TGA (无压缩)

### 内存管理

- **手动管理**: 用户负责调用 `img_free()` 释放图像
- **句柄系统**: 通过带代际校验的不透明整数句柄访问图像对象，默认容量为 1024
- **写时复制**: 变换操作返回新图像，不修改原图

### 算法实现

- **缩放**: 最近邻插值（Nearest Neighbor）
- **灰度化**: 加权平均 (0.299R + 0.587G + 0.114B)
- **亮度**: 线性调整 `pixel + delta`
- **对比度**: 以 128 为中心缩放 `(pixel - 128) * factor + 128`

## 性能特征

### 优点
- 零依赖：stb 为单头文件库
- 内存高效：按需加载，手动管理
- 启动快：插件动态加载

### 限制
- 缩放质量：仅支持最近邻插值
- 单线程：无并行处理
- 无 GPU 加速

### 典型性能
- 加载 1920x1080 JPEG: ~50ms
- 缩放到 50%: ~20ms
- 保存为 PNG: ~100ms

（以上数据为参考值，实际性能取决于硬件）

## 使用场景

### ✅ 适合
- 批量图像格式转换
- 缩略图生成
- 简单的图像预处理
- 自动化图像处理流程
- 原型开发和脚本化任务

### ❌ 不适合
- 实时视频处理
- 高质量图像缩放（需双线性/双三次插值）
- 复杂滤镜效果（模糊、锐化等）
- 图像分割、识别等 AI 任务

## 扩展建议

如需更高级功能，可考虑：

1. **高质量缩放**: 集成 `stb_image_resize` 或 `stb_image_resize2`
2. **滤镜效果**: 添加卷积核操作
3. **高级变换**: 任意角度旋转、透视变换
4. **元数据**: EXIF 读取（需 `libexif`）
5. **动画支持**: GIF/APNG 多帧处理

## 编译说明

### 前置条件
```bash
# 确保 vcpkg 已安装 stb
vcpkg install stb
```

### 构建
```bash
cd build
cmake ..
cmake --build . --target img
cmake --build . --target img_plugin
```

### 测试
```bash
ctest -R test_img -V
```

### 安装
```bash
cmake --install . --component img
```

## 维护信息

- **创建时间**: 2026-06-13
- **最后更新**: 2026-06-13
- **维护者**: TurboScript Team
- **许可证**: 与 stb 相同 (MIT / Public Domain)

## 问题反馈

如遇到问题，请提供：
1. TurboScript 版本
2. 操作系统和编译器版本
3. 图像格式和尺寸
4. 完整错误信息或崩溃日志

## 参考资料

- stb_image: https://github.com/nothings/stb
- TurboScript 文档: `docs/`
- 插件开发指南: `docs/PLUGIN_SYSTEM.md`
