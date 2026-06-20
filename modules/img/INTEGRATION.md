# IMG 模块集成说明

本文档说明如何将 IMG 模块集成到 TurboScript 项目文档中。

## 1. 构建系统集成

### 已完成的集成

✅ **modules/CMakeLists.txt**
```cmake
add_subdirectory(img)
```
已添加到构建系统。

✅ **vcpkg.json**
```json
{
  "name": "stb"
}
```
stb 依赖已存在。

### 验证构建

```bash
# 重新配置
cd build
cmake ..

# 编译模块
cmake --build . --target img
cmake --build . --target img_plugin

# 运行测试
ctest -R test_img -V
```

## 2. 文档集成建议

### docs/modules/IMG_MODULE.md

建议在 `docs/modules/` 创建 IMG 模块文档：

```markdown
# IMG 图像处理模块

基于 stb_image 的轻量级图像处理模块。

## 快速开始

\`\`\`javascript
import("img");

let img = img_load("photo.jpg", 0);
let resized = img_resize(img, 800, 600);
img_save(resized, "output.png", 0, 0);
img_free(resized);
img_free(img);
\`\`\`

详细文档见: `modules/img/README.md`
```

### docs/api/api-reference.md

在 API 参考中添加 IMG 模块章节：

```markdown
## IMG - 图像处理

### 核心函数

- `img_load(path, channels)` - 加载图像
- `img_save(handle, path, format, quality)` - 保存图像
- `img_resize(handle, width, height)` - 缩放
- ...

完整 API: [IMG Module](../modules/IMG_MODULE.md)
```

### docs/getting-started.md

在快速开始中添加示例：

```markdown
### 图像处理示例

\`\`\`javascript
import("img");

// 生成缩略图
let img = img_load("photo.jpg", 0);
let thumb = img_resize(img, 200, 150);
img_save(thumb, "thumb.jpg", 1, 85);
img_free(thumb);
img_free(img);
\`\`\`
```

## 3. 示例集成

### examples/img_processing.ts

建议在 `examples/` 目录创建完整示例：

```javascript
// examples/img_processing.ts
import("img");

// 批量生成缩略图
function batch_thumbnails(inputs, output_dir, max_size) {
    for (let i = 0; i < len(inputs); i = i + 1) {
        let path = inputs[i];
        let img = img_load(path, 0);
        
        if (img != 0) {
            let w = img_width(img);
            let h = img_height(img);
            let scale = max_size / (w > h ? w : h);
            
            let thumb = img_resize(img, int(w * scale), int(h * scale));
            let out_path = output_dir + "/" + basename(path);
            
            img_save(thumb, out_path, 1, 85);
            img_free(thumb);
            img_free(img);
            
            print("处理完成: " + path);
        }
    }
}
```

## 4. 测试集成

### 集成测试

确保 IMG 模块的测试被包含在 CI/CD 流程中：

```yaml
# .github/workflows/test.yml
- name: Test IMG module
  run: |
    cd build
    ctest -R test_img --output-on-failure
```

### 测试数据

建议在 `test/data/images/` 目录放置测试图像：
- `test.jpg` - JPEG 测试图
- `test.png` - PNG 测试图
- `test_gray.jpg` - 灰度图

## 5. README 更新

### 主 README.md

在特性列表中添加：

```markdown
## 核心特性

- ...
- **图像处理**: 基于 stb_image，支持多种格式的读写和变换
- ...

## 模块列表

| 模块 | 说明 | 文档 |
|------|------|------|
| ...  | ...  | ...  |
| img  | 图像处理 | [README](modules/img/README.md) |
```

## 6. 插件加载测试

### 运行时验证

```javascript
// test_img_plugin.ts
import("img");

print("IMG 模块已加载");

let img = img_create(100, 100, 3);
if (img != 0) {
    print("✓ 模块工作正常");
    img_free(img);
} else {
    print("✗ 模块加载失败");
}
```

运行：
```bash
turbo_script test_img_plugin.ts
```

## 7. 版本说明

在 CHANGELOG 中记录：

```markdown
## [未发布]

### 新增
- IMG 模块：图像处理功能
  - 支持 JPEG、PNG、BMP、TGA 等格式
  - 提供缩放、裁剪、翻转、旋转等变换
  - 亮度、对比度调整和灰度转换
  - 像素级读写访问
```

## 8. 构建验证清单

- [ ] `cmake ..` 配置成功
- [ ] `cmake --build . --target img` 编译成功
- [ ] `cmake --build . --target img_plugin` 编译成功
- [ ] `ctest -R test_img` 测试通过
- [ ] 插件文件存在于 `build/bin/` 或 `build/lib/`
- [ ] TurboScript 能成功 `import("img")`
- [ ] 基本功能测试通过

## 9. 常见问题

### Q: 找不到 stb_image.h

**A**: 确保 vcpkg 已安装 stb：
```bash
vcpkg install stb
```

### Q: 插件加载失败

**A**: 检查插件路径：
```bash
# 插件应在此目录
ls build/bin/img_plugin.*
```

### Q: 编译时符号未定义

**A**: 确保链接了必要的库：
```cmake
target_link_libraries(img PUBLIC exprtk TurboNet::Utils)
```

## 10. 下一步

集成完成后，可以考虑：

1. 添加更多图像处理算法（模糊、锐化等）
2. 支持高质量缩放（双线性/双三次插值）
3. 添加批处理辅助函数
4. 性能基准测试
5. 与其他模块的集成（如与 fin 模块结合做图表生成）

---

**完成时间**: 2026-06-13
**集成状态**: 待验证
