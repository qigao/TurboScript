#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

// 包含生成的头文件
#include "test_optional_v4.h"

int main() {
    printf("=== TBE可选字段功能测试 ===\n\n");
    
    // 测试1: 验证常量定义
    printf("1. 常量定义:\n");
    printf("   User_OPTIONAL_FIELD_COUNT = %d\n", User_OPTIONAL_FIELD_COUNT);
    printf("   User_PRESENCE_BITMAP_SIZE = %d\n", User_PRESENCE_BITMAP_SIZE);
    printf("   User_age_DEFAULT = %d\n", User_age_DEFAULT);
    printf("   User_BLOCK_LENGTH = %d\n", User_BLOCK_LENGTH);
    printf("   User_id_OFFSET = %d\n", User_id_OFFSET);
    printf("   User_age_OFFSET = %d\n", User_age_OFFSET);
    printf("\n");
    
    // 测试2: 创建消息缓冲区
    uint8_t buffer[256];
    memset(buffer, 0, sizeof(buffer));
    
    User_builder_t builder;
    if (!User_builder_bind(&builder, buffer, sizeof(buffer))) {
        printf("错误：无法绑定构建器\n");
        return 1;
    }
    
    printf("2. 构建器绑定成功\n");
    
    // 测试3: 设置必需字段
    User_id_set(&builder, 12345);
    printf("3. 设置 id = 12345\n");
    
    // 测试4: 设置可选字段
    User_age_set(&builder, 30);
    printf("4. 设置 age = 30\n");
    
    // 测试5: 创建视图并读取
    User_view_t view;
    if (!User_view_bind(&view, buffer, builder.size)) {
        printf("错误：无法绑定视图\n");
        return 1;
    }
    
    printf("5. 视图绑定成功\n");
    
    // 测试6: 读取字段
    uint32_t id = User_id_get(&view);
    printf("6. 读取 id = %u\n", id);
    
    uint32_t age = User_age_get(&view);
    printf("7. 读取 age = %u\n", age);
    
    // 测试7: 测试可选字段检查
    bool has_age = User_has_age(&view);
    bool has_email = User_has_email(&view);
    printf("8. 可选字段检查:\n");
    printf("   has_age = %s\n", has_age ? "true" : "false");
    printf("   has_email = %s\n", has_email ? "true" : "false");
    
    // 测试8: 测试默认值函数
    uint32_t age_with_default = User_age_get_default(&view);
    uint32_t age_with_custom = User_age_get_or(&view, 25);
    printf("9. 默认值函数:\n");
    printf("   age_get_default() = %u\n", age_with_default);
    printf("   age_get_or(25) = %u\n", age_with_custom);
    
    printf("\n=== 测试完成 ===\n");
    return 0;
}