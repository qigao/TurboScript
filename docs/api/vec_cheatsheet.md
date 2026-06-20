# TurboScript Vector (vec) Cheat Sheet

面向日常开发的向量操作速查表。`vec` 是 TurboScript 提供的高效 SIMD 加速向量处理库。
许多函数同时支持标准调用 `vec.func(v)` 和点号调用 `v.func()`。

> **提示:** `vec` 模块中的函数如果不在顶层调用，请确保先 `import("vec")` 或在 C++ 侧通过 `turbo_script_load_plugin(ctx, "vec")` 加载。

## 聚合与统计

| 函数 | 别名 (点号支持) | 说明 | 示例 |
|---|---|---|---|
| `vec.len(v)` | `v.length()`, `v.size()` | 获取向量/字符串的长度 | `[1, 2, 3].length() // 3` |
| `vec.sum(v)` | `v.sum()` | 向量元素之和 | `vec.sum([1, 2, 3]) // 6` |
| `vec.avg(v)` | `v.avg()`, `v.mean()`| 向量元素平均值 | `[10, 20, 30].avg() // 20` |
| `vec.min(v)` | `v.min()` | 返回向量中的最小值 | `vec.min([4, 2, 8]) // 2` |
| `vec.max(v)` | `v.max()` | 返回向量中的最大值 | `[4, 2, 8].max() // 8` |

## 构造与组合

| 函数 | 说明 | 示例 |
|---|---|---|
| `vec.range(end)` | 生成 `[0, end)` 的递增向量区间 | `vec.range(5) // [0, 1, 2, 3, 4]` |
| `vec.range(start, end)` | 生成 `[start, end)` 的递增区间 | `vec.range(2, 5) // [2, 3, 4]` |
| `vec.concat(v1, v2)` | 拼接两个向量获得一个新的向量 | `vec.concat([1, 2], [3, 4]) // [1, 2, 3, 4]`|

## 排序与重组

**注意:** 排序操作返回**新向量**，不会修改原向量。

| 函数 | 别名 (点号支持) | 说明 | 示例 |
|---|---|---|---|
| `vec.sort(v)` | `v.sort()` | 返回升序排序的新向量 | `[3, 1, 2].sort() // [1, 2, 3]` |
| `vec.sort_desc(v)` | - | 返回降序排序的新向量 | `vec.sort_desc([1, 4, 3]) // [4, 3, 1]`|
| `vec.reverse(v)` | `v.reverse()` | 返回反转后的新向量 | `[1, 2, 3].reverse() // [3, 2, 1]` |
| `vec.unique(v)` | - | 移除重复项，返回升序排序的新向量 | `vec.unique([3, 1, 1, 2, 3]) // [1, 2, 3]`|

## 搜索与分析

| 函数 | 别名 (点号支持) | 说明 | 示例 |
|---|---|---|---|
| `vec.find(v, val)` | `v.indexOf(val)` | 查找 `val` 首次出现的索引（找不到返回 -1）| `[10, 20, 30].indexOf(20) // 1` |
| `vec.cumsum(v)` | `v.cumsum()` | 计算累计和 (Cumulative sum) 序列 | `vec.cumsum([1, 2, 3]) // [1, 3, 6]`|
| `vec.diff(v)` | - | 计算前向差分（相邻元素之差） | `vec.diff([1, 3, 6]) // [2, 3]` |

## 组合示例

```js
import("vec");

// 1) 价格数据处理流水线
var prices = [100.5, 98.2, 102.1, 100.5, 99.0];
var sorted = prices.sort();          // [98.2, 99.0, 100.5, ...
var u_prices = vec.unique(sorted);   // 剔除重复的 100.5

// 2) 收益率计算与分析
var changes = vec.diff(prices);      // 每日价差
var avg_change = changes.avg();      // 均值变化

// 3) 生成指定序列并计算累计进度
var steps = vec.range(1, 6);         // [1, 2, 3, 4, 5]
var progress = steps.cumsum();       // [1, 3, 6, 10, 15]
```

## 性能提示

- `vec` 模块的底层实现在计算 `sum`, `min`, `max` 及 `diff` 时针对大数组（8个阈值以上）提供了原生 **AVX2 / SIMD (Single Instruction Multiple Data)** 加速。
- 对于涉及密集数学聚合的操作，优先使用 `vec.xxx()` 而不是在 TurboScript 层进行显式的 `for` 循环遍历。
