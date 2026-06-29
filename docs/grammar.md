
# ExprTk / TurboScript Language Grammar Reference

A TypeScript/JavaScript-like scripting language built on a custom Lemon/re2c parser.
Used as the language frontend for **TurboScript** (`turbo_script_ctx_t`), a high-performance embeddable scripting engine that lowers scripts to MIR and supports both MIR interpreter and MIR JIT execution from the same IR.

TurboScript is intentionally smaller than TypeScript/JavaScript. It focuses on host embedding, data binding, text processing, math/matrix computation, time-series analysis, technical indicators, and finance-oriented scripting.

---

## Table of Contents

1. [Module Architecture](#module-architecture)
2. [Syntax Overview](#syntax-overview)
3. [Data Types](#data-types)
4. [Operators](#operators)
5. [Variables & Assignment](#variables--assignment)
6. [Control Flow](#control-flow)
7. [Functions & Modules](#functions--modules)
8. [Object-Oriented Programming](#object-oriented-programming)
9. [Built-in Modules](#built-in-modules)
10. [Built-in Functions — Math & Statistics](#built-in-functions--math--statistics)
11. [Built-in Functions — Linear Algebra](#built-in-functions--linear-algebra)
12. [Built-in Functions — String](#built-in-functions--string)
13. [Built-in Functions — Vector](#built-in-functions--vector)
14. [Safety & Resource Limits](#safety--resource-limits)

---

## Module Architecture

TurboScript uses a unified registry for function dispatch. Functions can be:
1.  **Global Built-ins**: (e.g., `sin`, `cos`, `avg`, `sum`).
2.  **Environment Registered**: Native C functions registered via `exprtk_env_register_func`.
3.  **Module-based**: Functions grouped into modules (e.g., `json.query`, `csv.col`).
4.  **User Functions**: Defined in the script using the `func` keyword.

Lookup priority:
`Local Scope` → `Environment Functions` → `Module-based Functions` → `Global Registry`.

---

## Syntax Overview

- **Semicolons**: Optional at the end of statements.
- **Comments**: 
  - `// Single line comment`
  - `/* Multi-line comment */`
- **Case Sensitivity**: Identifiers are case-sensitive.
- **Blocks**: Grouped by curly braces `{ ... }`.

---

## Data Types

1.  **Number**: 64-bit floating point (e.g., `3.14`, `-0.5`, `1e10`).
2.  **String**: UTF-8 string literal (`"hello"`) and template string (`` `hello` ``).
3.  **Int64**: 64-bit integer literal/runtime value (e.g., `42`).
4.  **Bool**: Native boolean (`true`, `false`).
5.  **Bytes**: Binary byte buffer returned by `bytes(s)`.
6.  **UUID**: 128-bit UUID value returned by `uuid(text)`, `uuid4()`, or `uuid7()`.
7.  **Datetime**: Parsed date-time value returned by `datetime(text)` or `datetime.parse(text)`.
8.  **Date**: Calendar date returned by `date.parse(text)`.
9.  **Time**: Time-of-day returned by `time.parse(text)`.
10. **Duration**: Millisecond duration returned by `duration.parse(text)`.
11. **Vector**: Array of numbers (e.g., `[1, 2, 3]`).
12. **Map**: Explicit script key-value container (e.g., `map{name: "Alice", age: 30}`).
13. **Object**: Host/parser/data_bind plain record object with field access.
14. **List**: Heterogeneous array of any values (e.g., `list("hello", 42, [1,2])`).
15. **Null**: Absence of a value (`null` or `nil`).

### Built-in Constants

| Name | Value | Description |
|---|---|---|
| `pi` | 3.14159265358979… | π |
| `e` | 2.71828182845904… | Euler's number |
| `inf` | ∞ | Positive infinity |
| `nan` | NaN | Not a Number |
| `true` | true | Boolean true |
| `false` | false | Boolean false |
| `null` / `nil` | null | Null value |

### Type Introspection

| Function | Returns |
|----------|---------|
| `typeof(x)` | `"number"`, `"int64"`, `"bool"`, `"bytes"`, `"uuid"`, `"datetime"`, `"date"`, `"time"`, `"duration"`, `"string"`, `"vector"`, `"map"`, `"object"`, `"list"`, `"null"` |
| `is_number(x)` | `1.0` if number, else `0.0` |
| `is_int64(x)` | `1.0` if int64, else `0.0` |
| `is_bool(x)` | `1.0` if bool, else `0.0` |
| `is_bytes(x)` | `1.0` if bytes, else `0.0` |
| `is_uuid(x)` | `1.0` if UUID, else `0.0` |
| `is_datetime(x)` | `1.0` if datetime, else `0.0` |
| `is_date(x)` | `1.0` if date, else `0.0` |
| `is_time(x)` | `1.0` if time, else `0.0` |
| `is_duration(x)` | `1.0` if duration, else `0.0` |
| `is_string(x)` | `1.0` if string, else `0.0` |
| `is_vector(x)` | `1.0` if vector, else `0.0` |
| `is_map(x)` | `1.0` if map, else `0.0` |
| `is_object(x)` | `1.0` if plain object, else `0.0` |
| `is_list(x)` | `1.0` if list, else `0.0` |
| `is_null(x)` | `1.0` if null, else `0.0` |

```js
typeof(42)          // → "int64"
typeof(42.5)        // → "number"
typeof(true)        // → "bool"
typeof(bytes("Az")) // → "bytes"
typeof(uuid("01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001")) // → "uuid"
typeof(datetime("Sat, 04 Mar 2006 13:27:54 GMT")) // → "datetime"
typeof(date.parse("2026-06-28")) // → "date"
typeof(time.parse("09:30:05"))   // → "time"
typeof(duration.parse("1h"))     // → "duration"
typeof("hello")     // → "string"
typeof([1,2,3])     // → "vector"
typeof(map{a: 1})   // → "map"
typeof(null)        // → "null"
is_number(42)       // → 1
is_string(42)       // → 0
is_uuid(uuid4())    // → 1
uuid_string(uuid7()) // → "019..."
datetime("Sat, 04 Mar 2006 13:27:54 GMT").year // → 2006
```

---

## Operators

| Type | Operators |
|------|-----------|
| Arithmetic | `+`, `-`, `*`, `/`, `%` (mod), `^` (power) |
| Comparison | `==`, `!=`, `<>`, `<`, `<=`, `>`, `>=` |
| Logic | `and`, `or`, `not`, `&&`, `||`, `!` |
| Assignment | `=`, `+=`, `-=`, `*=`, `/=` |
| Vector | `[]` (indexing), `[a..b]` (slicing) |
| Range | `..` (start..end) |
| Member | `.` (namespace or method access) |
| Optional Chaining | `?.` (safe member access, returns null if LHS is null) |
| Pipe | `\|>` (pipe LHS as first argument to RHS function) |
| Spread | `...` (spread elements) |
| Ternary | `? :` (conditional expression) |
| Arrow | `=>` (arrow function definition) |

---


### Two layers of functions

| Layer | How registered | Examples |
|---|---|---|
| **Global registry** | `exprtk_module_t` sorted array — always available | `sin`, `sum`, `read_file`, `now`, `date`, `os_name` |
| **TurboScript plugins** | `import("name")` 或 `turbo_script_load_plugin(ctx, "name")` 动态加载 | `csv.*`, `json.*`, `ta.*`, `ts.*`, `vec.*`, `http.*`, `strategy.*`, `data_bind.*` |
---

## Variables & Assignment

Variables are dynamically typed. Use `var` or `let` for explicit declaration (interchangeable). `const` creates a read-only variable.

```js
var x = 10;
let y = [1, 2, 3];
const PI = 3.14159;

x += 5;   // compound assignment
y[0] = 4; // vector element update
```

### Destructuring Assignment

You can extract values from vectors and maps using a concise destructuring syntax:

```js
// Extracted into variables
var [a, b] = [10, 20];
const map{name, age} = map{name: "Alice", age: 30};

// Destructuring an existing variable
[x, y] = vec.range(0, 2);
```

### Function Definition (`=` with call LHS)

When the left side of `=` is a call-expression with identifier arguments, it defines a function:

```
f(x)    = x * 2;          // single parameter
area(w, h) = w * h;       // multiple parameters
```

---

## Control Flow

### If / Else
```js
if (a > b) {
    print("a is greater");
} elif (a == b) {
    print("equals");
} else {
    print("b is greater");
}
```

### Ternary Operator
```js
label = x > 0 ? "positive" : "negative";
```

### While / For Loops
```js
var i = 0;
while (i < 10) {
    i += 1;
    if (i == 5) continue;
    if (i == 8) break;
}

for (var i = 0; i < 10; i += 1) { print(i); }

for (x in [10, 20, 30]) { print(x); }
```

### Switch Statement
```js
switch (status) {
    case 1:  print("Active");
    case 2:  print("Pending");
    default: print("Unknown");
}
```

### Error Handling (try / catch / throw)

Structured error recovery for graceful failure handling.

```js
try {
    if (b == 0) throw "division by zero";
} catch (e) {
    print("Error: " + e);
}
```

---

## Functions & Modules

### User Defined Functions
Functions are defined using the `func` keyword. They support recursion and local scoping.

```js
func fib(n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}
```

### Anonymous Functions and Arrow Functions (Lambdas)

TurboScript supports modern anonymous functions and arrow expressions, which are essentially **Lambda expressions**, ideal for passing logic as variables or callbacks.

```js
// Anonymous function (lambda)
var square = func(x) { return x * x; };

// Arrow functions (lambdas)
var log_prefix = (msg) => "[INFO] " + msg;
var cube = (x) => x * x * x;
var data = () => map{id: 1};

// Immediately invoked arrow function
( (x) => print(x) )(100);
```

### Default Arguments

Parameters can have default values using `=`. Defaults are evaluated when the argument is omitted.

```js
func greet(name = "world") {
    return "hello " + name;
}
greet()         // → "hello world"
greet("Alice")  // → "hello Alice"
```

### Optional Chaining (`?.`)

Safe member access that returns `null` instead of error when the object is null.

```js
user = map{name: "Alice", address: map{city: "NYC"}};
city = user?.address?.city;  // → "NYC"
```

### Pipe Operator (`|>`)

Pipes the left-hand value as the first argument to the right-hand function call.

```js
// Equivalent to: ta.rsi(ta.sma(prices, 14), 14)
prices |> ta.sma(14) |> ta.rsi(14);
```

### Import
Load and execute external script files.
```js
import("utils.tbs");
```

---

## Object-Oriented Programming

Class and interface declarations are statement-level declarations; do not prefix them with `var` or `let`.

```js
class Counter {
    private value = 0;
    protected static seed = 10;

    constructor(start) { this.value = start; }
    add(delta) { this.value = this.value + delta; return this.value; }
    static base() { return Counter.seed; }
}

interface Shape {
    area();
}
```

Class body members:

| Form | Meaning |
|---|---|
| `name;` | Public instance field initialized to `null` |
| `name = expr;` | Public instance field with a default value |
| `static name = expr;` | Static field on the class value |
| `public name = expr;` | Explicit public field |
| `protected name = expr;` | Field visible to the declaring class and subclasses |
| `private name = expr;` | Field visible only to the declaring class |
| `method(args) { ... }` | Instance method |
| `static method(args) { ... }` | Static method |
| `abstract method(args);` | Abstract method requirement |
| `static method(args);` | Static interface method requirement |

Access modifiers may be used with methods and fields. Dynamic `_name` private members are still recognized for compatibility, but explicit `private` / `protected` declarations are preferred for new code.

Core OOP expressions:

| Form | Meaning |
|---|---|
| `Class(args)` / `new Class(args)` | Construct an instance |
| `obj.field` / `obj.method(args)` | Instance member access / call |
| `Class.field` / `Class.method(args)` | Static member access / call |
| `super(args)` | Parent constructor call |
| `super.method(args)` / `super.field` | Parent member access from a subclass method |
| `value instanceof ClassOrInterface` | Runtime class or interface identity check |

Methods may be overloaded by arity and by declared class/interface parameter types. JIT/MIR lowers OOP operations through runtime helpers and rejects unsupported forms rather than silently falling back to the interpreter.

---

## Built-in Modules

### IO Module — File Operations (global)
All IO functions are registered globally via `exprtk_module_io()`. No `import` or namespace prefix needed.

| Script function | Behaviour |
|---|---|
| `read_file(path)` | Read file contents as string (or 0 on failure) |
| `write_file(path, content)` | Write string to file → 0 on success |
| `append_file(path, content)` | Append string to file → 0 on success |
| `copy_file(src, dst)` | Copy file contents → 0 on success |
| `file_exists(path)` | 1.0 if path exists, else 0.0 |
| `file_size(path)` | Size in bytes (or -1 on failure) |
| `file_stat(path)` | Vector `[size, mtime_s, is_file, is_dir]` (or 0) |
| `file_truncate(path, length)` | Truncate or extend a file → 0 on success |
| `is_file(path)` | 1.0 if regular file, else 0.0 |
| `is_dir(path)` | 1.0 if directory, else 0.0 |
| `file_remove(path)` | Delete file → 0 on success |
| `file_rename(old, new)` | Rename/move file → 0 on success |

### IO Module — Directory Operations (global)

| Script function | Behaviour |
|---|---|
| `listdir(path)` | Directory entry names as a list of strings |
| `glob(pattern)` | Matching paths as a list of strings |
| `mkdir(path [, mode])` | Create directory → 0 on success |
| `mkdir_recursive(path [, mode])` | Create directory and missing parents → 0 on success |
| `rmdir(path)` | Remove directory → 0 on success |
| `rmdir_recursive(path)` | Remove directory tree → 0 on success |
| `tmpdir()` | Returns temporary directory path string |

### IO Module — Path Utilities (global)

| Script function | Behaviour |
|---|---|
| `path_join(base, rel)` | Join path components → string |
| `path_dirname(path)` | Directory component → string |
| `path_basename(path)` | Filename component → string |
| `path_is_absolute(path)` | 1.0 if absolute path, else 0.0 |

### IO Module — Date / Time (global)

| Script function | Behaviour |
|---|---|
| `now()` | Current Unix timestamp (seconds) |
| `date(str)` | Parse date string with TurboNet datetime parser → Unix timestamp |
| `date_utc(str)` | Parse fixed UTC date/time text → Unix timestamp |
| `format_date(ts [, fmt])` | Format timestamp → string (default RFC 822; custom format uses host local time) |
| `format_date_utc(ts [, fmt])` | Format timestamp in UTC → string |

The core runtime and parser module expose native datetime functions:
`datetime(text)`, `datetime.parse(text)`, `datetime.to_time(value)`,
`datetime.timestamp(value)`, and `datetime.format_rfc822(timestamp)`.
Datetime values support field access such as `year`, `month`, `day`, `hour`,
`minute`, `second`, `tz_offset`, `has_tz`, `day_of_week`, and `timestamp`.
Native date/time/duration values are exposed through `date.parse(text)`,
`time.parse(text)`, and `duration.parse(text)`. `date` supports `year`,
`month`, and `day`; `time` supports `hour`, `minute`, `second`, and
`millisecond`; `duration` supports `milliseconds`/`ms` and `seconds`.

### IO Module — Platform Info (global)

| Script function | Behaviour |
|---|---|
| `os_name()` | Returns `"windows"`, `"linux"`, `"macos"`, or `"unknown"` |
| `pid()` | Current process ID |
| `uptime_ms()` | Milliseconds since process start |
| `monotonic_ms()` | Monotonic clock in milliseconds |

---

### Vector Arithmetic

Vector operations are **element-wise**. Scalar broadcasting is supported.

```
[1, 2, 3] + [4, 5, 6]  // → [5, 7, 9]
[1, 2, 3] * 10          // → [10, 20, 30]
[10, 20] - [3, 4]       // → [7, 16]
```

### Indexing & Slicing

```
v = [10, 20, 30, 40, 50];
v[0]       // → 10.0  (0-based)
v[1..4]    // → [20, 30, 40]  (exclusive end)

s = "hello";
s[1..4]    // → "ell"
```

---

## Variables & Assignment

The language uses standard assignment:

| Token | Lexeme | Valid context |
|-------|--------|---------------|
| `ASSIGN` | `=` | Variable assignment OR function definition |
| `VAR / LET`| `var / let` | Optional declaration keywords |
| `EQ` | `==` | Equality comparison (never assignment) |

### Basic Assignment (`=`)

The primary assignment operator. Works for both variables and function definitions.

```js
x = 42;
name = "Alice";
prices = [100, 102, 104];
```

### `var` and `let` keywords

`var` and `let` are interchangeable and optional. They can be used to declare a variable.

```js
var x = 10;
let msg = "hi";
x = 20; // Re-assignment without keyword
```

### Semicolons

Semicolons are used as statement separators. However, they are **optional** after any block-expression (like `if`, `while`, `func`).

```js
func f(x) {
    return x * 2;
} // No semicolon needed here

f(10) // No semicolon needed for the last statement in a script
```

### Function Definition (`=` with call LHS)

When the left side of `=` is a call-expression with identifier arguments, it defines a function:

```
f(x)    = x * 2;          // single parameter
area(w, h) = w * h;       // multiple parameters
```

The grammar dispatches on the left node type at parse time:

- `VARIABLE = expr` → assignment
- `VARIABLE(VARIABLE, ...) = expr` → function definition

### Compound Assignment

```
x = 10;
x += 5;   // x = 15  (sugar for x = x + 5)
x -= 3;   // x = 12
x *= 2;   // x = 24
x /= 4;   // x = 6
```

### Comparison: `==` vs `=`

```
x = 10;    // assigns 10 to x
x == 10;    // evaluates to 1.0 (true), does NOT assign
```

### Alternative logical operators

The lexer accepts both word and symbolic forms — they are identical tokens:

| Word form | Symbolic form |
|-----------|---------------|
| `and`     | `&&`          |
| `or`      | `\|\|`        |
| `not`     | `!`           |

```
1 and 0   // ≡  1 && 0  → 0
1 or 0    // ≡  1 || 0  → 1
not 0     // ≡  !0      → 1
```

### Variable Persistence

Variables persist across multiple MIR executions within the same `turbo_script_ctx_t`.

```c
turbo_script_run(ctx, "x = 100;");
turbo_script_run(ctx, "y = x + 50;");  // y = 150 ✓
```

---

## Control Flow

### `if` / `else`

```
if (condition) { ... } else { ... }
if (condition) { ... }              // else is optional
```

```
x = if (score > 80) { "A" } else { "B" };
```

---

## Built-in Functions — Math & Statistics

| Function                                    | Returns  | Description                              |
|---------------------------------------------|----------|------------------------------------------|
| `clamp(x, lo, hi)`                          | number   | Clamp x into [lo, hi]                    |
| `saturate(x)`                               | number   | Clamp x into [0, 1]                      |
| `step(edge, x)`                             | number   | 0 if x < edge, else 1                    |
| `lerp(a, b, t)`                             | number   | Linear interpolation                     |
| `inverse_lerp(a, b, x)`                     | number   | Solve t where x = lerp(a, b, t)          |
| `remap(x, in0, in1, out0, out1)`            | number   | Map range [in0,in1] to [out0,out1]       |
| `smoothstep(edge0, edge1, x)`               | number   | Hermite-smoothed interpolation           |
| `radians(deg)` / `degrees(rad)`             | number   | Angle conversion                         |
| `fract(x)`                                  | number   | Fractional part                          |
| `cbrt(x)`                                   | number   | Cube root                                |
| `hypot(x, y)`                               | number   | Stable `sqrt(x*x + y*y)`                 |
| `log1p(x)` / `expm1(x)`                     | number   | Accurate near zero                       |
| `exp2(x)`                                   | number   | 2^x                                      |
| `logn(x, base)`                             | number   | Logarithm with arbitrary base            |
| `copysign(x, y)`                            | number   | Magnitude of x with sign of y            |
| `is_nan(x)` / `is_inf(x)`                   | number   | Predicate (1.0 true, 0.0 false)          |
| `relu(x)` / `sigmoid(x)` / `softplus(x)`    | number   | Common ML activation functions           |
| `fibonacci(n)`                              | number   | n-th Fibonacci number                    |
| `gcd(a, b)`                                 | number   | Greatest common divisor                  |
| `normal_rand(mu, sigma)`                    | number   | Sample from N(μ, σ²)                    |
| `median(v)`                                 | number   | Median of vector                         |
| `percentile(v, p)`                          | number   | p-th percentile (0–100)                  |
| `geometric_mean(v)`                         | number   | Geometric mean                           |
| `harmonic_mean(v)`                          | number   | Harmonic mean                            |
| `skewness(v)`                               | number   | Sample skewness                          |
| `kurtosis(v)`                               | number   | Sample excess kurtosis                   |
| `stats.normal_pdf(x[, mu, sigma])`          | number   | Normal probability density               |
| `stats.normal_cdf(x[, mu, sigma])`          | number   | Normal cumulative distribution           |
| `stats.normal_quantile(p[, mu, sigma])`     | number   | Inverse normal CDF                       |
| `stats.t_pdf(x, df)`                        | number   | Student t probability density            |
| `stats.t_cdf(x, df)`                        | number   | Student t cumulative distribution        |
| `stats.t_quantile(p, df)`                   | number   | Inverse Student t CDF                    |
| `stats.t_test_1samp(v, mean)`               | map      | One-sample t-test result: `t`, `df`, `p` |

---

## Built-in Functions — Linear Algebra

Matrices are stored **column-major** in flat vectors, matching the `miniblas/linalg`
backend used by core matrix operations.
`miniblas/linalg` backed functions accept an optional final layout flag:
omit it or pass `0` for column-major storage, or pass `1` for Eigen-style
row-major storage.

| Function                                    | Returns  | Description                              |
|---------------------------------------------|----------|------------------------------------------|
| `det2(m)`                                   | number   | Determinant of 2×2 matrix               |
| `det3(m)`                                   | number   | Determinant of 3×3 matrix               |
| `inv2(m)`                                   | vector   | Inverse of 2×2 matrix (4 elements)      |
| `inv3(m)`                                   | vector   | Inverse of 3×3 matrix (9 elements)      |
| `matrix.add(A, B, rows, cols[, layout])`    | vector   | Elementwise matrix addition             |
| `matrix.sub(A, B, rows, cols[, layout])`    | vector   | Elementwise matrix subtraction          |
| `matrix.hadamard(A, B, rows, cols[, layout])` / `matrix.mul(...)` | vector | Elementwise matrix product |
| `matrix.scale(A, rows, cols, scalar[, layout])` | vector | Scale every matrix element              |
| `matmul(A, B, m, k, n[, layout])`           | vector   | Matrix multiply: (m×k) × (k×n)          |
| `matrix.copy(A, rows, cols[, layout])`      | vector   | Copy a flat matrix vector               |
| `matrix.flatten(A, rows, cols[, layout])`   | vector   | Return a flat matrix copy               |
| `matrix.reshape(A, old_rows, old_cols, new_rows, new_cols[, layout])` | vector | Validate shape-compatible reshape |
| `matrix.slice(A, rows, cols, row0, row_count, col0, col_count[, layout])` | vector | Extract a matrix block |
| `matrix.identity(n[, layout])`              | vector   | Create an n×n identity matrix           |
| `matrix.eye_like(A, rows, cols[, layout])`  | vector   | Create an identity-shaped matrix        |
| `matrix.dot(a, b)`                          | number   | Vector dot product                      |
| `matrix.outer(a, b[, layout])`              | vector   | Vector outer product                    |
| `matrix.add_row(A, row, rows, cols[, layout])` | vector | Broadcast-add a row vector              |
| `matrix.sub_row(A, row, rows, cols[, layout])` | vector | Broadcast-subtract a row vector         |
| `matrix.mul_row(A, row, rows, cols[, layout])` | vector | Broadcast-multiply a row vector         |
| `matrix.div_row(A, row, rows, cols[, layout])` | vector | Broadcast-divide by a row vector        |
| `matrix.add_col(A, col, rows, cols[, layout])` | vector | Broadcast-add a column vector           |
| `matrix.sub_col(A, col, rows, cols[, layout])` | vector | Broadcast-subtract a column vector      |
| `matrix.mul_col(A, col, rows, cols[, layout])` | vector | Broadcast-multiply a column vector      |
| `matrix.div_col(A, col, rows, cols[, layout])` | vector | Broadcast-divide by a column vector     |
| `matrix.sum(A, rows, cols[, axis][, layout])` | number/vector | Sum all elements, or reduce by axis |
| `matrix.mean(A, rows, cols[, axis][, layout])` | number/vector | Mean of all elements, or reduce by axis |
| `matrix.min(A, rows, cols[, axis][, layout])` | number/vector | Minimum of all elements, or reduce by axis |
| `matrix.max(A, rows, cols[, axis][, layout])` | number/vector | Maximum of all elements, or reduce by axis |
| `matrix.variance(A, rows, cols[, axis][, layout])` / `matrix.var(...)` | number/vector | Population variance |
| `matrix.std(A, rows, cols[, axis][, layout])` | number/vector | Population standard deviation |
| `matrix.argmin(A, rows, cols[, axis][, layout])` | number/vector | Index of minimum value |
| `matrix.argmax(A, rows, cols[, axis][, layout])` | number/vector | Index of maximum value |
| `matrix.row(A, rows, cols, index[, layout])` | vector | Extract one row                         |
| `matrix.col(A, rows, cols, index[, layout])` | vector | Extract one column                      |
| `matrix.det(A, n[, layout])`                | number   | Generic n×n determinant                 |
| `matrix.diag(A, rows, cols[, layout])`      | vector   | Extract the main diagonal               |
| `matrix.zeros(rows, cols[, layout])`        | vector   | Create a dense zero matrix              |
| `matrix.ones(rows, cols[, layout])`         | vector   | Create a dense matrix filled with `1`   |
| `matrix.full(rows, cols, value[, layout])`  | vector   | Create a dense matrix filled with value |
| `matrix.inv(A, n[, layout])`                | vector   | Generic n×n inverse                     |
| `matrix.norm(A, rows, cols[, kind][, layout])` | number | Matrix norm: Frobenius, `"l1"`, `"inf"` |
| `matrix.rank(A, rows, cols[, tol][, layout])` | number | Numerical matrix rank                   |
| `matrix.cond(A, rows, cols[, layout])`      | number   | 2-norm condition estimate               |
| `matrix.slogdet(A, n[, layout])`            | vector   | `[sign, log(abs(det))]` through LU      |
| `matrix.solve(A, B, n, cols[, layout])`     | vector   | Solve `A * X = B`                       |
| `matrix.shape(A, rows, cols[, layout])`     | vector   | Validate and return `[rows, cols]`      |
| `matrix.info(A, rows, cols[, layout])`      | map      | Matrix metadata                         |
| `matrix.rows(A, rows, cols[, layout])`      | number   | Validate and return row count           |
| `matrix.cols(A, rows, cols[, layout])`      | number   | Validate and return column count        |
| `matrix.trace(A, rows, cols[, layout])`     | number   | Sum the main diagonal                   |
| `matrix.transpose(A, rows, cols[, layout])` | vector   | Transpose preserving declared layout    |
| `transpose(m, rows, cols)`                  | vector   | Transpose matrix                         |
| `eig2(m)`                                   | vector   | Eigenvalues of 2×2 matrix               |
| `eig3(m, out)`                              | number   | Eigenvalues of 3×3 into out vector      |
| `trace2(m)`                                 | number   | Trace of 2×2 matrix                     |
| `linalg.qr(A, rows, cols[, layout])`        | list     | `[Q, R]` QR decomposition               |
| `linalg.lu(A, n[, layout])`                 | list     | `[L, U, pivots]` LU decomposition       |
| `linalg.lu_solve(A, B, n, cols[, layout])`  | vector   | Solve linear systems through LU         |
| `linalg.solve_cholesky(A, B, n, cols[, layout])` | vector | Solve SPD systems through Cholesky |
| `linalg.det_lu(A, n[, layout])`             | number   | Determinant through LU                  |
| `linalg.rank(A, rows, cols[, tol][, layout])` | number | Alias for `matrix.rank`                 |
| `linalg.cond(A, rows, cols[, layout])`      | number   | Alias for `matrix.cond`                 |
| `linalg.eigh(A, n[, layout])`               | vector   | Symmetric eigenvalues, ascending        |
| `linalg.pinv(A, rows, cols[, layout])`      | vector   | Pseudoinverse for square or tall full-rank matrices |
| `linalg.lstsq(A, b, rows, cols[, layout])`  | vector   | Least-squares solution                  |
| `linalg.svd(A, rows, cols[, layout])` / `linalg.svd2(...)` | vector | Singular values sorted descending |
| `linalg.eig2(A)`                            | vector   | 2×2 eigenvalues alias                   |

Most `matrix.*` helpers also have global `mat_*` aliases, including
`mat_copy`, `mat_flatten`, `mat_reshape`, `mat_slice`, `mat_dot`, `mat_outer`,
`mat_identity`, `mat_eye_like`, `mat_var`, `mat_std`, `mat_argmin`, `mat_argmax`,
and the existing arithmetic/reduction aliases.
For `matrix.norm(A, rows, cols, x)`, a numeric `x` is interpreted as the layout flag;
use string kinds such as `"l1"` or `"inf"` when passing four arguments.
For matrix reductions, the fourth argument is always the axis; pass layout as the fifth
argument: `matrix.sum(A, rows, cols, 0, 1)`.
Prefer `matrix.variance(...)` or `mat_var(...)` in source text because `var` is
also a declaration keyword.

```
det2([1, 2, 3, 4])   // column-major [1 3; 2 4], 1*4 - 3*2 = -2

// [1 3; 2 4] * [5 7; 6 8]
matmul([1, 2, 3, 4], [5, 6, 7, 8], 2, 2, 2)
// → [23, 34, 31, 46]

// Explicit row-major input/output, like Eigen::RowMajor
matmul([1, 2, 3, 4], [5, 6, 7, 8], 2, 2, 2, 1)
// → [19, 22, 43, 50]

matrix.hadamard([1, 2, 3, 4], [10, 20, 30, 40], 2, 2)
// → [10, 40, 90, 160]

matrix.sum([1, 4, 2, 5, 3, 6], 2, 3, "col")
// → [5, 7, 9]

matrix.row([1, 2, 3, 4, 5, 6], 2, 3, 1, 1)
// → [4, 5, 6]

matrix.info([1, 2, 3, 4], 2, 2).order
// → "col"

matrix.full(2, 3, 7)
// → [7, 7, 7, 7, 7, 7]

matrix.det([1, 0, 5, 2, 1, 6, 3, 4, 0], 3)
// → 1

matrix.norm([1, -4, -2, 5, 3, -6], 2, 3, "l1")
// → 9

matrix.diag([1, 3, 5, 2, 4, 6], 3, 2)
// → [1, 4]

matrix.trace([1, 3, 5, 2, 4, 6], 3, 2)
// → 5

matrix.transpose([1, 2, 3, 4, 5, 6], 2, 3, 1)
// → [1, 4, 2, 5, 3, 6]

matrix.solve([2, 1, 1, 3], [5, 10], 2, 1)
// → [1, 3]

linalg.lu([4, 2, 2, 3], 2)
// → [L, U, pivots]

matrix.rank([1, 2, 2, 4], 2, 2)
// → 1

linalg.eigh([2, 1, 1, 2], 2)
// → [1, 3]

linalg.svd([1, 0, 0, 0, 2, 0], 3, 2)
// → [2, 1]

matrix.slice([1, 2, 3, 4, 5, 6], 2, 3, 0, 2, 1, 2)
linalg.svd([3, 0, 0, 4], 2, 2)
stats.t_test_1samp([1, 2, 3, 4], 2.5).df
```

---

## Built-in Functions — Table

Table helpers operate on `list(map{...})` values.

| Function                                    | Returns  | Description                              |
|---------------------------------------------|----------|------------------------------------------|
| `table.select(rows, field...)`              | list     | Project selected fields from each row    |
| `table.filter(rows, predicate)`             | list     | Keep rows accepted by a script function or function name |
| `table.groupby(rows, key, value[, op])`     | map      | Aggregate by key; `op` is `"sum"`, `"mean"`, or `"count"` |
| `table.join(left, right, key)`              | list     | Inner join two row lists by key          |

```
var rows = list(map{id:1, group:"a", value:10}, map{id:2, group:"a", value:20});
var grouped = table.groupby(rows, "group", "value", "mean");
grouped.a.value  // → 15
```

---

## Built-in Functions — String

| Function                          | Returns  | Description                                    |
|-----------------------------------|----------|------------------------------------------------|
| `"a" + "b"`                       | string   | Concatenation                                  |
| `len(s)`                          | number   | String length in bytes                         |
| `lower(s)`                        | string   | Lowercase                                      |
| `upper(s)`                        | string   | Uppercase                                      |
| `trim(s)`                         | string   | Trim leading and trailing whitespace           |
| `ltrim(s)`                        | string   | Trim leading whitespace                        |
| `rtrim(s)`                        | string   | Trim trailing whitespace                       |
| `substr(s, start, len)`           | string   | Substring (0-based start)                      |
| `reverse(s)`                      | string   | Reverse string                                 |
| `replace(s, from, to)`            | string   | Replace all occurrences                        |
| `replace_all(s, from, to)`        | string   | Alias for replace                              |
| `replace_range(s, start, len, v)` | string   | Replace a byte range                           |
| `insert(s, index, value)`         | string   | Insert at a byte index                         |
| `delete_range(s, start, len)`     | string   | Delete a byte range                            |
| `repeat(s, count)`                | string   | Alias for `str_repeat`                         |
| `count(s, sub)`                   | number   | Count non-overlapping substring occurrences    |
| `find_all(s, sub)`                | list     | Non-overlapping byte indexes of a substring    |
| `find_all_overlapping(s, sub)`    | list     | Overlapping byte indexes of a substring        |
| `count_overlapping(s, sub)`       | number   | Count overlapping substring occurrences        |
| `left(s, count)` / `str_take(s, count)` | string | Take leading UTF-8 codepoints when valid   |
| `right(s, count)`                 | string   | Take trailing UTF-8 codepoints when valid      |
| `str_drop(s, count)`              | string   | Drop leading UTF-8 codepoints when valid       |
| `normalize_space(s)`              | string   | Trim and collapse ASCII whitespace             |
| `center(s, width [, fill])`       | string   | Center text; UTF-8 valid strings use codepoints |
| `truncate(s, max_len [, suffix])` | string   | Truncate text; UTF-8 valid strings use codepoints |
| `zfill(s, width)`                 | string   | Left-pad with zeros after an optional sign     |
| `expand_tabs(s [, tabsize])`      | string   | Expand tabs to spaces                          |
| `chomp(s)`                        | string   | Remove one trailing line ending                |
| `split_lines(s)`                  | list     | Alias for `str_lines`                          |
| `line_count(s)`                   | number   | Count logical lines like `str_lines`           |
| `indent(s, prefix [, first])`     | string   | Prefix lines; optional first-line flag         |
| `dedent(s)` / `unindent(s)`       | string   | Remove common leading spaces/tabs              |
| `surround(s, prefix [, suffix])`  | string   | Add paired prefix/suffix                       |
| `unwrap(s, prefix [, suffix])`    | string   | Remove paired prefix/suffix if both match      |
| `word_wrap(s, width [, break_long])` | string | Wrap ASCII words to a byte width              |
| `shorten(s, width [, suffix])`    | string   | Shorten at a word boundary                     |
| `snake_case(s)` / `kebab_case(s)` | string   | Convert ASCII words to delimited field names   |
| `camel_case(s)` / `pascal_case(s)` | string  | Convert ASCII words to camel/Pascal case       |
| `slugify(s)`                      | string   | Convert ASCII words to a URL slug              |
| `strip(s)` / `lstrip(s)` / `rstrip(s)` | string | Trim aliases                              |
| `strip_prefix(s, prefix)`         | string   | Remove prefix if present                       |
| `strip_suffix(s, suffix)`         | string   | Remove suffix if present                       |
| `padStart(s, width [, fill])`     | string   | JS-style left padding alias                    |
| `padEnd(s, width [, fill])`       | string   | JS-style right padding alias                   |
| `split_whitespace(s)`             | list     | Split ASCII whitespace-separated words         |
| `split_once(s, delim)`            | list     | Split once at first delimiter                  |
| `rsplit_once(s, delim)`           | list     | Split once at last delimiter                   |
| `split_limit(s, delim, maxsplit)` | list     | Split from the left with a split limit         |
| `rsplit(s, delim [, maxsplit])`   | list     | Split from the right                           |
| `contains(s, sub)`                | number   | 1 if s contains sub, else 0                   |
| `starts_with(s, prefix)`          | number   | 1 if s starts with prefix                     |
| `ends_with(s, suffix)`            | number   | 1 if s ends with suffix                       |
| `index_of(s, sub)`                | number   | First index of sub in s (-1 if not found)     |
| `last_index_of(s, sub)`           | number   | Last index of sub in s (-1 if not found)      |
| `find(s, sub [, start])`          | number   | Forward search with optional start byte       |
| `rfind(s, sub [, end])`           | number   | Reverse search with optional exclusive end    |
| `slice(s, start [, end])`         | string   | Byte slice; negative indices count from end   |
| `char_at(s, index)`               | string   | Single byte at index as a string              |
| `byte_at(s, index)`               | number   | Byte value at index                           |
| `byte_length(s)`                  | number   | Byte length alias                             |
| `utf8_slice(s, start [, end])`    | string   | UTF-8 codepoint slice with negative indices   |
| `ord(s [, index])`                | number   | UTF-8 codepoint value                         |
| `chr(codepoint)`                  | string   | UTF-8 string from one codepoint               |
| `from_codepoint(cp...)`           | string   | UTF-8 string from codepoints                  |
| `codepoint_at(s, index)`          | number   | Alias for indexed `ord`                       |
| `is_empty(s)`                     | number   | 1 if s has zero bytes                         |
| `is_blank(s)`                     | number   | 1 if s is empty or ASCII whitespace only      |
| `is_ascii(s)`                     | number   | 1 if all bytes are ASCII                      |
| `is_digit(s)`                     | number   | 1 if non-empty ASCII digits only              |
| `is_hex(s)`                       | number   | 1 if non-empty ASCII hex digits only          |
| `is_alpha(s)`                     | number   | 1 if non-empty ASCII letters only             |
| `is_alnum(s)`                     | number   | 1 if non-empty ASCII letters/digits only      |
| `is_space(s)`                     | number   | 1 if non-empty ASCII whitespace only          |
| `is_printable(s)`                 | number   | 1 if ASCII printable or common whitespace     |
| `is_lower(s)`                     | number   | 1 if lowercase-only among cased ASCII chars   |
| `is_upper(s)`                     | number   | 1 if uppercase-only among cased ASCII chars   |
| `constant_time_eq(a, b)`          | number   | Byte equality without early exit              |
| `html_escape(s)`                  | string   | Escape HTML-sensitive characters              |
| `html_unescape(s)`                | string   | Decode common HTML entities                   |
| `json_escape(s)`                  | string   | Escape JSON string literal content            |
| `json_unescape(s)`                | string   | Decode JSON string literal escapes            |
| `url_encode(s)`                   | string   | Percent-encode UTF-8 bytes                    |
| `url_decode(s)`                   | string   | Decode percent escapes                        |
| `base64_encode(s)`                | string   | Base64 encode bytes                           |
| `base64_decode(s)`                | string   | Base64 decode bytes into a string             |
| `base64url_encode(s)`             | string   | URL-safe Base64 encode without padding        |
| `base64url_decode(s)`             | string   | URL-safe Base64 decode into a string          |
| `hex_encode(s)`                   | string   | Lowercase hex encode bytes                    |
| `hex_decode(s)`                   | string   | Hex decode bytes into a string                |
| `bytes(s)`                        | bytes    | Binary byte buffer                            |
| `from_bytes(bytes/list/vector)`   | string   | Build a string from byte values               |
| `template_render(t, data)`        | string   | Render a Mustache template from script data   |
| `template_render(t, data, parts)` | string   | Render with partial templates from a map      |
| `mustache_render(t, data)`        | string   | Alias for `template_render`                   |
| `s[start..end]`                   | string   | Slice (exclusive end, 0-based)                |

```js
lower("HeLLo")              // → "hello"
"HeLLo".lower()             // → "hello" (dot-style)
"hello world".substr(6, 5)  // → "world"
"banana".replace("a", "o")  // → "bonono"
"hello"[1..4]               // → "ell"
```

### Mustache Template Rendering

`template_render(template, data [, partials])` renders Mustache syntax against
native TurboScript values. Maps provide field lookup and dotted paths; lists and
vectors drive sections; scalars render as text; functions act as Mustache
lambdas. `{{name}}` escapes HTML-sensitive characters, while `{{{name}}}` writes
raw output. Missing fields and missing partials render empty text.
Partial names may use dotted paths into the partial map.

```js
data = map{
  name: "Ada",
  items: list(map{name: "one"}, map{name: "two"})
};
partials = map{item: "{{name}};"};
template_render("{{#items}}{{>item}}{{/items}}", data, partials)
// → "one;two;"
```

### Dot-Style String Methods

Strings support dot-style member calls. The expression `s.method(args)` is equivalent to `method(s, args)`.

| Dot-style | Equivalent | Returns |
|---|---|---|
| `s.length()` / `s.size()` | `len(s)` | number |
| `s.indexOf(sub)` | `index_of(s, sub)` | number |
| `s.substr(start [, len])` | `substr(s, start, len)` | string |
| `s.toUpper()` | `upper(s)` | string |
| `s.toLower()` | `lower(s)` | string |
| `s.upper()` | `upper(s)` | string |
| `s.lower()` | `lower(s)` | string |
| `s.trim()` | `trim(s)` | string |
| `s.ltrim()` | `ltrim(s)` | string |
| `s.rtrim()` | `rtrim(s)` | string |
| `s.reverse()` | `reverse(s)` | string |
| `s.replace(from, to)` | `replace(s, from, to)` | string |
| `s.contains(sub)` | `contains(s, sub)` | number |
| `s.starts_with(prefix)` | `starts_with(s, prefix)` | number |
| `s.ends_with(suffix)` | `ends_with(s, suffix)` | number |
| `s.split(delim)` | `split(s, delim)` | vector |

```js
name = "Hello World";
name.length()              // → 11
name.toLower()             // → "hello world"
name.indexOf("World")      // → 6
name.contains("World")     // → 1
name.substr(0, 5)          // → "Hello"
name.replace("World", "TurboScript")  // → "Hello TurboScript"
```

---

## Built-in Functions — Vector

| Function                          | Returns  | Description                                    |
|-----------------------------------|----------|------------------------------------------------|
| `[a, b, c]`                       | vector   | Vector literal                                 |
| `v[i]`                            | number   | Element access (0-based)                       |
| `v[s..e]`                         | vector   | Slice (exclusive end, 0-based)                |
| `size(v)`                         | number   | Number of elements                             |
| `len(v)`                          | number   | Alias for `size`                               |
| `avg(v)`                          | number   | Average of all elements                        |
| `sum(v)`                          | number   | Sum of all elements                            |
| `min(v)`                          | number   | Minimum element                                |
| `max(v)`                          | number   | Maximum element                                |
| `sort(v)`                         | vector   | Sorted copy (ascending)                        |
| `split(s, delim)`                 | vector   | Split string into numeric vector               |
| `vector_find_all(v, val)`         | vector   | All indices of val in v                        |
| `vector_find_value(v, val)`       | number   | First index of val in v (-1 if not found)      |

```js
v = [10, 20, 30, 20, 10];
idx = vector_find_value(v, 20);     // → 1.0 (first occurrence)
all_idxs = vector_find_all(v, 20);  // → [1.0, 3.0] (all indices)
```

```js
v = [1, 2, 3, 4, 5];
v[2]       // → 3.0
v[1..4]    // → [2, 3, 4]
avg(v)     // → 3.0
v.avg()    // → 3.0 (dot-style)
sum(v)     // → 15.0
v.sum()    // → 15.0 (dot-style)

// TurboScript: parse CSV into vector
prices = split("100,102,104", ",");
prices[0]  // → 100.0
```

### Dot-Style Vector Methods

Vectors support dot-style member calls. The expression `v.method(args)` is equivalent to `method(v, args)`.

Built-in methods (direct implementation):

| Dot-style | Returns | Description |
|---|---|---|
| `v.length()` / `v.size()` | number | Vector size |
| `v.push(x)` | number | Append element, mutates in-place, returns new size |
| `v.pop()` | number | Remove and return last element, mutates in-place |
| `v.indexOf(val)` | number | First index of value (-1 if not found) |
| `v.reverse()` | vector | Reversed copy |

Registry-dispatched methods (any registered function accepting a vector):

| Dot-style | Equivalent | Returns |
|---|---|---|
| `v.sum()` | `sum(v)` | number |
| `v.avg()` / `v.mean()` | `avg(v)` | number |
| `v.min()` | `min(v)` | number |
| `v.max()` | `max(v)` | number |
| `v.sort()` | `sort(v)` | vector |
| `v.median()` | `median(v)` | number |
| `v.skewness()` | `skewness(v)` | number |
| `v.kurtosis()` | `kurtosis(v)` | number |
| `v.cumsum()` | `cumsum(v)` | vector |

The dispatch chain tries: `vec_method` → `stats.method` → `math.method` → global registry. When the `fin` module is loaded, `ta.method` and `ts.method` are also checked (see `module.fin.md`).

```js
prices = [100, 102, 98, 105, 103];
prices.push(107);           // prices = [100, 102, 98, 105, 103, 107]
prices.length()             // → 6
prices.avg()                // → 102.5
prices.sort()               // → [98, 100, 102, 103, 105, 107]
prices.indexOf(98)          // → 2
last = prices.pop();        // last = 107, prices shrinks
```

---

## Safety & Resource Limits

The evaluator enforces hard limits to prevent out-of-memory or infinite execution.

| Limit | Default | Description |
|---|---|---|
| `max_recursion` | 1000 | Depth of user function calls |
| `max_loop_iterations` | 1000000 | Total iterations across all loops |
| `max_nodes` | ∞ | Total syntax nodes visited by runtime helpers |

### `assert(cond, msg)`
Aborts execution with a message if `cond` is false.
