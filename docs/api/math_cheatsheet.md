# TurboScript Math Cheat Sheet

面向日常开发的数学函数速查表。默认返回 `number`，布尔语义用 `1.0/0.0` 表示。

## 基础运算

| 函数 | 说明 | 示例 |
|---|---|---|
| `abs(x)` | 绝对值 | `abs(-3.5) // 3.5` |
| `sgn(x)` | 符号函数 | `sgn(-10) // -1` |
| `mod(a, b)` | 浮点取模 | `mod(7, 3) // 1` |
| `pow(x, y)` | 幂 | `pow(2, 8) // 256` |
| `sqrt(x)` | 平方根 | `sqrt(9) // 3` |
| `cbrt(x)` | 立方根 | `cbrt(-8) // -2` |
| `hypot(x, y)` | `sqrt(x*x+y*y)` 稳定写法 | `hypot(3, 4) // 5` |
| `copysign(x, y)` | 取 `x` 的绝对值并使用 `y` 的符号 | `copysign(3, -2) // -3` |
| `trunc(x)` | 向 0 截断 | `trunc(-3.9) // -3` |
| `round(x)` | 四舍五入 | `round(2.6) // 3` |
| `floor(x)` | 向下取整 | `floor(2.9) // 2` |
| `ceil(x)` | 向上取整 | `ceil(2.1) // 3` |
| `fract(x)` | 小数部分 `x-floor(x)` | `fract(3.25) // 0.25` |

## 指数与对数

| 函数 | 说明 | 示例 |
|---|---|---|
| `exp(x)` | `e^x` | `exp(1)` |
| `exp2(x)` | `2^x` | `exp2(10) // 1024` |
| `expm1(x)` | `exp(x)-1`（小值更稳定） | `expm1(1e-8)` |
| `log(x)` | 自然对数 | `log(e) // 1` |
| `log2(x)` | 2 为底对数 | `log2(8) // 3` |
| `log10(x)` | 10 为底对数 | `log10(1000) // 3` |
| `log1p(x)` | `log(1+x)`（小值更稳定） | `log1p(1e-8)` |
| `logn(x, base)` | 任意底对数 | `logn(8, 2) // 3` |

## 三角与角度

| 函数 | 说明 | 示例 |
|---|---|---|
| `sin(x)` `cos(x)` `tan(x)` | 三角函数（弧度） | `sin(pi / 2) // 1` |
| `asin(x)` `acos(x)` `atan(x)` | 反三角函数 | `asin(1) // pi/2` |
| `atan2(y, x)` | 双参数反正切 | `atan2(1, 1)` |
| `sinh(x)` `cosh(x)` `tanh(x)` | 双曲函数 | `tanh(0) // 0` |
| `radians(deg)` | 角度转弧度 | `radians(180) // pi` |
| `degrees(rad)` | 弧度转角度 | `degrees(pi) // 180` |

## 插值与区间映射

| 函数 | 说明 | 示例 |
|---|---|---|
| `clamp(x, lo, hi)` | 夹取到 `[lo, hi]` | `clamp(12, 0, 10) // 10` |
| `saturate(x)` | 夹取到 `[0, 1]` | `saturate(1.8) // 1` |
| `step(edge, x)` | 阶跃函数 | `step(0.5, 0.2) // 0` |
| `lerp(a, b, t)` | 线性插值 | `lerp(10, 20, 0.25) // 12.5` |
| `inverse_lerp(a, b, x)` | 反插值求 `t` | `inverse_lerp(10, 20, 12.5) // 0.25` |
| `remap(x, in0, in1, out0, out1)` | 区间映射 | `remap(5, 0, 10, 0, 100) // 50` |
| `smoothstep(edge0, edge1, x)` | 平滑 S 曲线 | `smoothstep(0, 1, 0.5) // 0.5` |

## 数值状态判断

| 函数 | 说明 | 示例 |
|---|---|---|
| `is_nan(x)` | 是否 NaN（`1/0`） | `is_nan(nan) // 1` |
| `is_inf(x)` | 是否 ±Inf（`1/0`） | `is_inf(inf) // 1` |

## 常见 ML 激活函数

| 函数 | 说明 | 示例 |
|---|---|---|
| `relu(x)` | `max(0, x)` | `relu(-2) // 0` |
| `sigmoid(x)` | `1 / (1 + exp(-x))` | `sigmoid(0) // 0.5` |
| `softplus(x)` | `log(1 + exp(x))` 的稳定版本 | `softplus(0) // 0.693...` |

## 组合示例

```js
// 1) 从传感器值做归一化+平滑
var t = inverse_lerp(20, 80, sensor_temp);
var score = smoothstep(0, 1, saturate(t));

// 2) 音量曲线（分贝 -> 线性）
var gain = pow(10, db / 20);

// 3) 风险阈值控制
var risk = clamp(raw_risk, 0, 1);
var alert = step(0.8, risk);   // >0.8 触发告警
```

## 边界行为提示

- `log/log2/log10/logn` 输入非法（如 `x<=0`）会返回非正常值或按函数实现返回 `0`；生产脚本建议先做范围判断。
- `inverse_lerp(a,b,x)` 在 `a==b` 时返回 `0`。
- `remap(x,in0,in1,...)` 在 `in0==in1` 时返回 `out0`。
- `is_nan/is_inf` 返回数字 `1.0/0.0`，不是独立布尔类型。
