// ============================================================================
// IMG 模块使用示例
// ============================================================================

import("img");

// ============================================================================
// 示例 1: 基本图像加载与保存
// ============================================================================

function example_basic() {
    print("示例 1: 基本图像加载与保存");
    
    // 加载图像 (0 = 自动检测通道数)
    let img = img_load("input.jpg", 0);
    
    if (img == 0) {
        print("  错误: 无法加载图像");
        return;
    }
    
    // 查询图像信息
    let w = img_width(img);
    let h = img_height(img);
    let c = img_channels(img);
    
    print("  图像尺寸: " + w + "x" + h);
    print("  通道数: " + c);
    
    // 保存为不同格式
    img_save(img, "output.png", 0, 0);    // PNG
    img_save(img, "output.jpg", 1, 90);   // JPEG (质量 90)
    img_save(img, "output.bmp", 2, 0);    // BMP
    
    img_free(img);
    print("  完成");
}

// ============================================================================
// 示例 2: 图像变换
// ============================================================================

function example_transform() {
    print("示例 2: 图像变换");
    
    let img = img_load("input.jpg", 0);
    if (img == 0) return;
    
    // 缩放到一半尺寸
    let w = img_width(img);
    let h = img_height(img);
    let resized = img_resize(img, w / 2, h / 2);
    img_save(resized, "resized.jpg", 1, 85);
    print("  缩放完成");
    
    // 裁剪中心区域
    let crop_size = 200;
    let cropped = img_crop(img, (w - crop_size) / 2, (h - crop_size) / 2, 
                           crop_size, crop_size);
    img_save(cropped, "cropped.jpg", 1, 85);
    print("  裁剪完成");
    
    // 翻转
    let h_flip = img_flip_horizontal(img);
    let v_flip = img_flip_vertical(img);
    img_save(h_flip, "flip_h.jpg", 1, 85);
    img_save(v_flip, "flip_v.jpg", 1, 85);
    print("  翻转完成");
    
    // 旋转
    let rotated = img_rotate_90(img);
    img_save(rotated, "rotated.jpg", 1, 85);
    print("  旋转完成");
    
    // 清理
    img_free(img);
    img_free(resized);
    img_free(cropped);
    img_free(h_flip);
    img_free(v_flip);
    img_free(rotated);
    
    print("  完成");
}

// ============================================================================
// 示例 3: 颜色处理
// ============================================================================

function example_color() {
    print("示例 3: 颜色处理");
    
    let img = img_load("input.jpg", 0);
    if (img == 0) return;
    
    // 灰度转换
    let gray = img_to_grayscale(img);
    img_save(gray, "gray.jpg", 1, 85);
    print("  灰度转换完成");
    
    // 亮度调整
    let bright = img_adjust_brightness(img, 0.3);   // 增加 30%
    let dark = img_adjust_brightness(img, -0.3);    // 降低 30%
    img_save(bright, "bright.jpg", 1, 85);
    img_save(dark, "dark.jpg", 1, 85);
    print("  亮度调整完成");
    
    // 对比度调整
    let high_contrast = img_adjust_contrast(img, 1.5);
    let low_contrast = img_adjust_contrast(img, 0.5);
    img_save(high_contrast, "high_contrast.jpg", 1, 85);
    img_save(low_contrast, "low_contrast.jpg", 1, 85);
    print("  对比度调整完成");
    
    // 清理
    img_free(img);
    img_free(gray);
    img_free(bright);
    img_free(dark);
    img_free(high_contrast);
    img_free(low_contrast);
    
    print("  完成");
}

// ============================================================================
// 示例 4: 像素级操作 - 创建渐变图
// ============================================================================

function example_gradient() {
    print("示例 4: 创建渐变图");
    
    let w = 400;
    let h = 300;
    let img = img_create(w, h, 3);
    
    // 创建水平渐变 (红->蓝)
    for (let y = 0; y < h; y = y + 1) {
        for (let x = 0; x < w; x = x + 1) {
            let r = int(255.0 * (1.0 - x / w));
            let g = 0;
            let b = int(255.0 * (x / w));
            
            img_set_pixel(img, x, y, 0, r);
            img_set_pixel(img, x, y, 1, g);
            img_set_pixel(img, x, y, 2, b);
        }
    }
    
    img_save(img, "gradient.png", 0, 0);
    img_free(img);
    
    print("  渐变图创建完成");
}

// ============================================================================
// 示例 5: 批量处理工作流
// ============================================================================

function process_pipeline(input_path, output_path) {
    // 加载原图
    let img = img_load(input_path, 0);
    if (img == 0) {
        print("  错误: 无法加载 " + input_path);
        return false;
    }
    
    // 管道处理流程
    // 1. 缩放到合适尺寸
    let w = img_width(img);
    let h = img_height(img);
    let max_dim = 1024;
    
    if (w > max_dim || h > max_dim) {
        let scale = max_dim / (w > h ? w : h);
        let new_w = int(w * scale);
        let new_h = int(h * scale);
        
        let temp = img_resize(img, new_w, new_h);
        img_free(img);
        img = temp;
    }
    
    // 2. 提升亮度和对比度
    let adjusted = img_adjust_brightness(img, 0.1);
    img_free(img);
    img = adjusted;
    
    adjusted = img_adjust_contrast(img, 1.2);
    img_free(img);
    img = adjusted;
    
    // 3. 保存结果
    let success = img_save(img, output_path, 1, 90);
    img_free(img);
    
    return success;
}

function example_batch() {
    print("示例 5: 批量处理");
    
    // 处理单张图片
    if (process_pipeline("photo1.jpg", "processed_photo1.jpg")) {
        print("  photo1.jpg 处理完成");
    }
    
    if (process_pipeline("photo2.jpg", "processed_photo2.jpg")) {
        print("  photo2.jpg 处理完成");
    }
    
    print("  批量处理完成");
}

// ============================================================================
// 示例 6: 图像分析 - 亮度直方图
// ============================================================================

function example_histogram() {
    print("示例 6: 亮度直方图分析");
    
    let img = img_load("input.jpg", 0);
    if (img == 0) return;
    
    // 转换为灰度
    let gray = img_to_grayscale(img);
    let w = img_width(gray);
    let h = img_height(gray);
    
    // 初始化直方图数组 (0-255)
    let histogram = vec(256);
    for (let i = 0; i < 256; i = i + 1) {
        histogram[i] = 0.0;
    }
    
    // 统计像素分布
    for (let y = 0; y < h; y = y + 1) {
        for (let x = 0; x < w; x = x + 1) {
            let val = img_get_pixel(gray, x, y, 0);
            histogram[val] = histogram[val] + 1.0;
        }
    }
    
    // 归一化
    let total = w * h;
    for (let i = 0; i < 256; i = i + 1) {
        histogram[i] = histogram[i] / total;
    }
    
    // 计算统计信息
    let mean = 0.0;
    for (let i = 0; i < 256; i = i + 1) {
        mean = mean + i * histogram[i];
    }
    
    print("  平均亮度: " + mean);
    
    // 找到峰值
    let max_val = 0.0;
    let peak = 0;
    for (let i = 0; i < 256; i = i + 1) {
        if (histogram[i] > max_val) {
            max_val = histogram[i];
            peak = i;
        }
    }
    
    print("  峰值亮度: " + peak + " (出现频率: " + (max_val * 100.0) + "%)");
    
    img_free(img);
    img_free(gray);
    
    print("  分析完成");
}

// ============================================================================
// 主程序
// ============================================================================

function main() {
    print("========================================");
    print("IMG 模块使用示例");
    print("========================================");
    
    // 运行各个示例
    // example_basic();
    // example_transform();
    // example_color();
    // example_gradient();
    // example_batch();
    // example_histogram();
    
    print("\n提示: 取消注释上面的函数调用以运行示例");
}

main();
