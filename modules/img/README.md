# IMG 模块 - 图像处理

基于 stb_image 库的图像处理模块，提供图像加载、保存、变换等功能。

## 特性

- **多格式支持**: JPEG, PNG, BMP, TGA 等常见格式
- **基本操作**: 缩放、裁剪、翻转、旋转
- **颜色调整**: 亮度、对比度调整、灰度转换
- **像素级访问**: 直接读写像素值
- **零依赖**: 基于 stb 单头文件库，无需外部依赖

## API 参考

### 图像加载与保存

```c
// 从文件加载图像
int handle = img_load("photo.jpg", 0);  // 0=自动检测通道数

// 创建空白图像
int handle = img_create(640, 480, 3);   // 宽、高、通道数

// 保存图像
img_save(handle, "output.png", 0, 0);   // format: 0=PNG, 1=JPEG, 2=BMP, 3=TGA

// 保存为 JPEG (指定质量)
img_save(handle, "output.jpg", 1, 85);  // 质量 1-100

// 释放图像
img_free(handle);
```

### 图像信息查询

```c
int w = img_width(handle);
int h = img_height(handle);
int c = img_channels(handle);
```

### 图像变换

```c
// 缩放
int resized = img_resize(handle, 320, 240);

// 裁剪 (x, y, width, height)
int cropped = img_crop(handle, 100, 100, 200, 200);

// 灰度转换
int gray = img_to_grayscale(handle);

// 翻转
int h_flip = img_flip_horizontal(handle);
int v_flip = img_flip_vertical(handle);

// 旋转 90 度（顺时针）
int rotated = img_rotate_90(handle);
```

### 颜色调整

```c
// 调整亮度 (-1.0 到 1.0)
int brighter = img_adjust_brightness(handle, 0.3);
int darker = img_adjust_brightness(handle, -0.3);

// 调整对比度 (0.0 到 2.0, 1.0=原始)
int high_contrast = img_adjust_contrast(handle, 1.5);
int low_contrast = img_adjust_contrast(handle, 0.5);
```

### 像素访问

```c
// 获取像素值 (返回 0-255)
int r = img_get_pixel(handle, x, y, 0);  // 红色通道
int g = img_get_pixel(handle, x, y, 1);  // 绿色通道
int b = img_get_pixel(handle, x, y, 2);  // 蓝色通道

// 设置像素值
img_set_pixel(handle, x, y, 0, 255);  // 设置红色通道
```

## 使用示例

### 示例 1: 批量图像处理

```javascript
import("img");

// 加载图像
let img = img_load("input.jpg", 0);

// 调整大小
let resized = img_resize(img, 800, 600);

// 应用滤镜效果
let adjusted = img_adjust_brightness(resized, 0.2);
adjusted = img_adjust_contrast(adjusted, 1.2);

// 保存结果
img_save(adjusted, "output.png", 0, 0);

// 清理
img_free(adjusted);
img_free(resized);
img_free(img);
```

### 示例 2: 创建缩略图

```javascript
import("img");

function create_thumbnail(input_path, output_path, max_size) {
    let img = img_load(input_path, 0);
    if (img == 0) {
        return false;
    }
    
    let w = img_width(img);
    let h = img_height(img);
    
    // 计算缩放比例
    let scale = max_size / (w > h ? w : h);
    let new_w = int(w * scale);
    let new_h = int(h * scale);
    
    // 缩放并保存
    let thumb = img_resize(img, new_w, new_h);
    let success = img_save(thumb, output_path, 1, 85);  // JPEG, 质量 85
    
    img_free(thumb);
    img_free(img);
    
    return success;
}

create_thumbnail("photo.jpg", "thumb.jpg", 200);
```

### 示例 3: 图像拼接

```javascript
import("img");

function concat_horizontal(path1, path2, output_path) {
    let img1 = img_load(path1, 3);  // 强制 RGB
    let img2 = img_load(path2, 3);
    
    let w1 = img_width(img1);
    let h1 = img_height(img1);
    let w2 = img_width(img2);
    let h2 = img_height(img2);
    
    // 统一高度
    let target_h = h1 < h2 ? h1 : h2;
    if (h1 != target_h) {
        let temp = img_resize(img1, int(w1 * target_h / h1), target_h);
        img_free(img1);
        img1 = temp;
        w1 = img_width(img1);
    }
    if (h2 != target_h) {
        let temp = img_resize(img2, int(w2 * target_h / h2), target_h);
        img_free(img2);
        img2 = temp;
        w2 = img_width(img2);
    }
    
    // 创建目标图像
    let result = img_create(w1 + w2, target_h, 3);
    
    // 逐像素复制（此处可优化为批量操作）
    for (let y = 0; y < target_h; y = y + 1) {
        for (let x = 0; x < w1; x = x + 1) {
            for (let c = 0; c < 3; c = c + 1) {
                let val = img_get_pixel(img1, x, y, c);
                img_set_pixel(result, x, y, c, val);
            }
        }
        for (let x = 0; x < w2; x = x + 1) {
            for (let c = 0; c < 3; c = c + 1) {
                let val = img_get_pixel(img2, x, y, c);
                img_set_pixel(result, w1 + x, y, c, val);
            }
        }
    }
    
    img_save(result, output_path, 0, 0);
    
    img_free(result);
    img_free(img1);
    img_free(img2);
}
```

### 示例 4: 图像分析

```javascript
import("img");

function analyze_brightness(path) {
    let img = img_load(path, 0);
    if (img == 0) return -1;
    
    // 转换为灰度
    let gray = img_to_grayscale(img);
    
    let w = img_width(gray);
    let h = img_height(gray);
    
    // 计算平均亮度
    let sum = 0.0;
    for (let y = 0; y < h; y = y + 1) {
        for (let x = 0; x < w; x = x + 1) {
            sum = sum + img_get_pixel(gray, x, y, 0);
        }
    }
    
    let avg = sum / (w * h);
    
    img_free(gray);
    img_free(img);
    
    return avg;
}

let brightness = analyze_brightness("photo.jpg");
print("平均亮度: " + brightness);
```

## 支持的图像格式

### 读取支持
- JPEG (baseline & progressive)
- PNG (1/2/4/8/16-bit-per-channel)
- TGA
- BMP (non-RLE)
- PSD (composited view only)
- GIF
- HDR (radiance rgbE format)
- PIC (Softimage PIC)
- PNM (PPM and PGM)

### 写入支持
- PNG
- JPEG
- BMP
- TGA

## 性能建议

1. **及时释放**: 处理完图像后立即调用 `img_free()` 释放内存
2. **批量操作**: 尽量减少中间图像的创建，链式调用时注意释放临时对象
3. **通道数**: 如果不需要透明通道，使用 3 通道 (RGB) 而非 4 通道 (RGBA)
4. **缩放算法**: 当前使用最近邻插值，速度快但质量一般；大尺寸缩放建议在外部工具完成

## 限制与注意事项

- 所有图像操作返回新图像句柄，原图像不会被修改
- 句柄是带代际校验的不透明整数，伪造、已释放或重复释放的句柄会被拒绝
- 默认最多同时持有 1024 个脚本图像句柄；达到上限时创建和变换函数返回 `0`
- 当前不支持动画 GIF 的多帧处理
- 旋转仅支持 90 度倍数，任意角度旋转需要额外实现
- 缩放采用最近邻插值，质量有限

## 编译

该模块依赖 stb_image 和 stb_image_write 头文件，已通过 vcpkg 集成：

```cmake
find_path(STB_INCLUDE_DIRS "stb_image.h")
target_include_directories(img PRIVATE ${STB_INCLUDE_DIRS})
```

## 测试

```bash
cd build
ctest -R test_img -V
```

## 许可

stb 库采用 MIT / Public Domain 双许可。
