# IMG 模块快速上手指南

## 一、快速开始

### 1. 在脚本中导入模块

```javascript
import("img");
```

### 2. 基本流程

```javascript
// 加载 -> 处理 -> 保存 -> 释放
let img = img_load("input.jpg", 0);
let processed = img_resize(img, 800, 600);
img_save(processed, "output.png", 0, 0);
img_free(processed);
img_free(img);
```

## 二、核心概念

### 图像句柄 (Handle)

所有图像操作都基于**句柄**（整数），它代表内存中的图像对象：

```javascript
let handle = img_load("photo.jpg", 0);  // 返回句柄
// handle 是一个整数，内部指向图像数据
```

### 内存管理

⚠️ **重要**: 必须手动释放不再使用的图像

```javascript
let img = img_load("photo.jpg", 0);
// ... 使用图像 ...
img_free(img);  // 释放内存！
```

### 非破坏性操作

所有变换函数返回**新图像**，原图像保持不变：

```javascript
let original = img_load("photo.jpg", 0);
let resized = img_resize(original, 400, 300);  // original 不变

// 需要分别释放两个图像
img_free(resized);
img_free(original);
```

## 三、常用操作速查

### 加载与保存

| 操作 | 函数 | 示例 |
|------|------|------|
| 加载图像 | `img_load(path, channels)` | `img_load("photo.jpg", 0)` |
| 创建空白 | `img_create(w, h, c)` | `img_create(640, 480, 3)` |
| 保存 PNG | `img_save(img, path, 0, 0)` | `img_save(img, "out.png", 0, 0)` |
| 保存 JPEG | `img_save(img, path, 1, quality)` | `img_save(img, "out.jpg", 1, 90)` |
| 释放图像 | `img_free(img)` | `img_free(img)` |

**通道数说明**:
- `0`: 自动检测
- `1`: 灰度
- `3`: RGB
- `4`: RGBA

### 查询信息

```javascript
let w = img_width(img);      // 宽度
let h = img_height(img);     // 高度
let c = img_channels(img);   // 通道数
```

### 几何变换

```javascript
// 缩放
let small = img_resize(img, 320, 240);

// 裁剪 (x, y, width, height)
let cropped = img_crop(img, 50, 50, 200, 200);

// 翻转
let h_flip = img_flip_horizontal(img);
let v_flip = img_flip_vertical(img);

// 旋转 90°
let rotated = img_rotate_90(img);
```

### 颜色调整

```javascript
// 灰度化
let gray = img_to_grayscale(img);

// 亮度: -1.0 (全黑) 到 1.0 (全白)
let brighter = img_adjust_brightness(img, 0.3);
let darker = img_adjust_brightness(img, -0.3);

// 对比度: 0.0 (平坦) 到 2.0 (高对比)
let high = img_adjust_contrast(img, 1.5);
let low = img_adjust_contrast(img, 0.5);
```

### 像素访问

```javascript
// 读取像素 (返回 0-255)
let r = img_get_pixel(img, x, y, 0);  // 红色
let g = img_get_pixel(img, x, y, 1);  // 绿色
let b = img_get_pixel(img, x, y, 2);  // 蓝色

// 写入像素
img_set_pixel(img, x, y, 0, 255);  // 设置红色=255
```

## 四、典型应用场景

### 场景 1: 生成缩略图

```javascript
function make_thumbnail(input, output, max_size) {
    let img = img_load(input, 0);
    let w = img_width(img);
    let h = img_height(img);
    
    let scale = max_size / (w > h ? w : h);
    let thumb = img_resize(img, int(w * scale), int(h * scale));
    
    img_save(thumb, output, 1, 85);
    img_free(thumb);
    img_free(img);
}
```

### 场景 2: 批量格式转换

```javascript
function convert_to_png(jpg_path, png_path) {
    let img = img_load(jpg_path, 0);
    if (img != 0) {
        img_save(img, png_path, 0, 0);
        img_free(img);
        return true;
    }
    return false;
}
```

### 场景 3: 自动修正

```javascript
function auto_enhance(input, output) {
    let img = img_load(input, 0);
    
    // 链式处理
    let temp = img_adjust_brightness(img, 0.1);
    img_free(img);
    
    img = img_adjust_contrast(temp, 1.2);
    img_free(temp);
    
    img_save(img, output, 1, 90);
    img_free(img);
}
```

### 场景 4: 水印添加

```javascript
function add_watermark(base_img, x, y, size) {
    // 在指定位置绘制白色方块
    for (let dy = 0; dy < size; dy = dy + 1) {
        for (let dx = 0; dx < size; dx = dx + 1) {
            img_set_pixel(base_img, x + dx, y + dy, 0, 255);
            img_set_pixel(base_img, x + dx, y + dy, 1, 255);
            img_set_pixel(base_img, x + dx, y + dy, 2, 255);
        }
    }
}
```

## 五、最佳实践

### ✅ 推荐做法

1. **及时释放内存**
   ```javascript
   let img = img_load("photo.jpg", 0);
   // 使用完毕立即释放
   img_free(img);
   ```

2. **检查加载结果**
   ```javascript
   let img = img_load("photo.jpg", 0);
   if (img == 0) {
       print("加载失败");
       return;
   }
   ```

3. **复用临时变量**
   ```javascript
   let img = img_load("photo.jpg", 0);
   let temp = img_resize(img, 800, 600);
   img_free(img);
   img = temp;  // 复用变量名
   ```

### ❌ 避免的做法

1. **忘记释放内存**
   ```javascript
   // 错误: 内存泄漏！
   for (let i = 0; i < 100; i = i + 1) {
       let img = img_load("photo.jpg", 0);
       // ... 但没有 img_free(img)
   }
   ```

2. **释放后继续使用**
   ```javascript
   let img = img_load("photo.jpg", 0);
   img_free(img);
   let w = img_width(img);  // 错误: 已释放！
   ```

3. **重复释放**
   ```javascript
   img_free(img);
   img_free(img);  // 错误: 二次释放！
   ```

## 六、性能提示

1. **批量操作**: 尽量减少中间图像创建
2. **通道选择**: 不需要透明度时用 RGB(3) 而非 RGBA(4)
3. **缩放策略**: 当前为最近邻插值，适合像素艺术；照片缩放可能需要外部工具
4. **像素遍历**: 访问大量像素时，尽量按行优先顺序（y 在外层循环）

## 七、格式支持

### 读取
✅ JPEG, PNG, BMP, TGA, GIF, PSD, HDR, PIC, PNM

### 写入
✅ JPEG, PNG, BMP, TGA

## 八、故障排查

### 问题: 加载返回 0

**可能原因**:
- 文件不存在或路径错误
- 文件格式不支持
- 文件损坏

**解决方法**:
```javascript
let img = img_load("photo.jpg", 0);
if (img == 0) {
    print("加载失败，请检查文件路径和格式");
}
```

### 问题: 保存失败

**可能原因**:
- 输出路径不存在
- 磁盘空间不足
- 权限不足

**解决方法**: 确保输出目录存在

### 问题: 内存占用过高

**原因**: 未及时释放图像

**解决方法**: 检查所有 `img_load` 和 `img_create` 是否有对应的 `img_free`

## 九、完整示例

```javascript
import("img");

function process_workflow() {
    // 1. 加载
    let img = img_load("input.jpg", 0);
    if (img == 0) {
        print("加载失败");
        return;
    }
    
    print("原始尺寸: " + img_width(img) + "x" + img_height(img));
    
    // 2. 缩放
    let resized = img_resize(img, 800, 600);
    img_free(img);  // 释放原图
    
    // 3. 调整亮度和对比度
    let adjusted = img_adjust_brightness(resized, 0.2);
    img_free(resized);
    
    let final = img_adjust_contrast(adjusted, 1.3);
    img_free(adjusted);
    
    // 4. 保存
    img_save(final, "output.jpg", 1, 90);
    print("处理完成");
    
    // 5. 清理
    img_free(final);
}

process_workflow();
```

## 十、参考资源

- 完整 API 文档: `README.md`
- 更多示例: `test/example_usage.ts`
- 单元测试: `test/test_img.c`

---

**提示**: 这是一个轻量级图像处理模块，适合简单的图像操作。复杂的图像处理（如高质量插值、复杂滤镜）建议使用专业工具。
