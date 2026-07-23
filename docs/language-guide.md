# TurboScript Language Guide

Complete reference for TurboScript syntax and language features.

---

## Table of Contents

1. [Data Types](#data-types)
2. [Variables and Constants](#variables-and-constants)
3. [Operators](#operators)
4. [Control Flow](#control-flow)
5. [Functions](#functions)
6. [Collections](#collections)
7. [Object-Oriented Programming](#object-oriented-programming)
8. [Modules and Imports](#modules-and-imports)
9. [Advanced Features](#advanced-features)
10. [Error Handling](#error-handling)
11. [Safety and Limits](#safety-and-limits)

---

## Data Types

TurboScript supports native scalar and container data types:

### Number

64-bit floating point numbers:

```javascript
var integer = 42;
var decimal = 3.14159;
var scientific = 1.5e10;
var negative = -273.15;
```

Integer literals are represented as native `int64` runtime values:

```javascript
var count = 42;
```

### Boolean

`true` and `false` are native `bool` values. They can still participate in numeric contexts as `1` and `0`.

### String

UTF-8 encoded strings with single or double quotes:

```javascript
var greeting = "Hello, World!";
var message = 'Single quotes work too';
var template = `Template strings`;
```

### Regular Expression

Use `regex.*` functions, `RegExp(...)`, or `/pattern/flags` literals for regular
expression matching:

```javascript
var phone = /\d{3}-\d{4}/;
var ok = phone.test("555-1234");          // 1
var pos = phone.search("call 555-1234");  // 5
var info = phone.exec("call 555-1234");   // map/object with matched span
var ci = /abc/i.test("ABC");              // 1
```

A regex literal is parsed as `RegExp("pattern", "flags")`, so it has the same
methods and limits as the existing `RegExp` object API. The `i` flag enables
case-insensitive matching. Use `\/` for a literal slash inside the pattern.
Arithmetic division remains `10 / 2`.

### Vector

Homogeneous arrays of numbers:

```javascript
var prices = [100, 102, 104, 103, 105];
var empty = [];
var range = [1, 2, 3, 4, 5];
```

### Map

Hash-based key-value pairs:

```javascript
var person = map{
    name: "Alice",
    age: 30,
    city: "NYC"
};

var nested = map{
    user: map{name: "Bob"},
    scores: [95, 87, 92]
};
```

### Plain Object

Plain objects are dynamic record values produced by host and parser modules.
For typed document mapping, declare a TurboScript class and use `mapper`:

```javascript
class User { name: string; age: int64; }
var user = mapper.read_json(User, "{\"name\":\"Alice\",\"age\":30}");
print(user.name);        // "Alice"
print(user["age"]);      // 30
```

Use class instances when fields need declared types and cross-format mapping.
Use `map` when the script itself needs an explicit key-value container.

### Datetime

Parsed date-time values are native `datetime` values:

```javascript
var dt = datetime("Sat, 04 Mar 2006 13:27:54 GMT");
dt.year;                 // 2006
datetime.to_time(dt);    // 1141478874
datetime_string(dt);     // RFC822/HTTP GMT text
```

### Date, Time, and Duration

Use `date.parse`, `time.parse`, and `duration.parse` when the script needs
native values instead of the legacy Unix timestamp returned by `date(str)`:

```javascript
var d = date.parse("2026-06-28");
d.year;                  // 2026
date.to_string(d);       // "2026-06-28"

var t = time.parse("09:30:05.123");
t.hour;                  // 9
time.to_string(t);       // "09:30:05.123"

var span = duration.parse("1h30m5s250ms");
span.milliseconds;       // 5405250
duration.seconds(span);  // 5405.25
duration.to_string(span);// "1:30:05.250"
```

### Decimal

Use `decimal.parse` for exact fixed-point values when binary floating point is
not appropriate:

```javascript
var price = decimal.parse("123.4500");
price.mantissa;              // 12345
price.scale;                 // 2
decimal.to_string(price);    // "123.45"
price.to_string();           // "123.45"
```

Decimal values compare after normalizing trailing fractional zeroes, so
`decimal.parse("123.4500") == decimal.parse("123.45")`.

### List

Heterogeneous collections (mixed types):

```javascript
var mixed = list("hello", 42, [1, 2, 3], map{key: "value"});
var item = mixed[0];  // "hello"
```

### Null

Represents absence of value:

```javascript
var empty = null;
var nothing = nil;  // equivalent to null
```

### Built-in Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `pi` | 3.14159265358979… | π (pi) |
| `e` | 2.71828182845904… | Euler's number |
| `inf` | ∞ | Positive infinity |
| `nan` | NaN | Not a Number |
| `true` | true | Boolean true |
| `false` | false | Boolean false |
| `null` / `nil` | null | Null value |

### Type Introspection

```javascript
typeof(42)          // "int64"
typeof(42.5)        // "number"
typeof(true)        // "bool"
typeof(bytes("Az")) // "bytes"
typeof(uuid4())     // "uuid"
typeof(datetime("Sat, 04 Mar 2006 13:27:54 GMT")) // "datetime"
typeof(date.parse("2026-06-28")) // "date"
typeof(time.parse("09:30:05"))   // "time"
typeof(duration.parse("1h"))     // "duration"
typeof(decimal.parse("123.45"))  // "decimal"
typeof("hello")     // "string"
typeof([1,2,3])     // "vector"
typeof(map{a: 1})   // "map"
// mapper.read_json(...) returns a typed class instance
typeof(null)        // "null"

is_number(42)       // 1 (true)
is_int64(42)        // 1 (true)
is_bool(true)       // 1 (true)
is_bytes(bytes("Az")) // 1 (true)
is_uuid(uuid4())    // 1 (true)
is_datetime(datetime("Sat, 04 Mar 2006 13:27:54 GMT")) // 1 (true)
is_date(date.parse("2026-06-28")) // 1 (true)
is_time(time.parse("09:30:05"))   // 1 (true)
is_duration(duration.parse("1h")) // 1 (true)
is_decimal(decimal.parse("123.45")) // 1 (true)
is_string("hi")     // 1 (true)
is_vector([1,2])    // 1 (true)
is_map(map{})       // 1 (true)
is_object(obj)      // 1 for host/parser plain objects
is_null(null)       // 1 (true)
```

---

## Variables and Constants

### Variable Declaration

```javascript
// Using 'var' keyword
var x = 10;
var name = "Alice";

// Using 'let' keyword (equivalent to var)
let y = 20;
let message = "Hello";

// Without keyword (implicit declaration)
z = 30;
```

### Constants

```javascript
const PI = 3.14159;
const MAX_SIZE = 1000;

// PI = 3.14;  // Error: cannot reassign constant
```

### Assignment Operators

```javascript
x = 10;      // Simple assignment
x += 5;      // x = x + 5  (15)
x -= 3;      // x = x - 3  (12)
x *= 2;      // x = x * 2  (24)
x /= 4;      // x = x / 4  (6)
```

### Destructuring Assignment

Extract values from vectors and maps:

```javascript
// Vector destructuring
var [a, b, c] = [10, 20, 30];
// a = 10, b = 20, c = 30

// Skip elements
var [first, , third] = [1, 2, 3];
// first = 1, third = 3

// Rest parameter
var [head, ...tail] = [1, 2, 3, 4, 5];
// head = 1, tail = [2, 3, 4, 5]

// Map destructuring
var map{name, age} = map{name: "Alice", age: 30};
// name = "Alice", age = 30

// Nested destructuring
var [[x, y], z] = [[1, 2], 3];
// x = 1, y = 2, z = 3
```

---

## Operators

### Arithmetic Operators

```javascript
x + y    // Addition
x - y    // Subtraction
x * y    // Multiplication
x / y    // Division
x % y    // Modulo
x ^ y    // Power (exponentiation)
```

### Comparison Operators

```javascript
x == y   // Equal
x != y   // Not equal
x <> y   // Not equal (alternative)
x < y    // Less than
x <= y   // Less than or equal
x > y    // Greater than
x >= y   // Greater than or equal
```

### Logical Operators

```javascript
// Word form
x and y  // Logical AND
x or y   // Logical OR
not x    // Logical NOT

// Symbol form (equivalent)
x && y   // Logical AND
x || y   // Logical OR
!x       // Logical NOT
```

### Special Operators

#### Ternary Operator

```javascript
var result = condition ? value_if_true : value_if_false;

var status = age >= 18 ? "Adult" : "Minor";
```

#### Pipe Operator

Pass value as first argument to next function:

```javascript
// Traditional nesting
var result = sum(filter(map(data, f), g));

// With pipe operator
var result = data |> map(f) |> filter(g) |> sum();

// Real example
prices |> ta.sma(20) |> ta.rsi(14);
// Equivalent to: ta.rsi(ta.sma(prices, 20), 14)
```

#### Optional Chaining

Safe property access that returns null instead of error:

```javascript
var user = map{
    name: "Alice",
    address: map{city: "NYC"}
};

var city = user?.address?.city;  // "NYC"
var zip = user?.address?.zip;    // null (no error)

var missing = null;
var value = missing?.property;   // null (no error)
```

#### Spread Operator

Expand elements in function calls or array literals:

```javascript
var arr = [2, 3, 4];
sum(1, ...arr, 5);  // sum(1, 2, 3, 4, 5)

var combined = [1, ...arr, 5];  // [1, 2, 3, 4, 5]
```

---

## Control Flow

### If Statement

```javascript
if (condition) {
    // code
}

if (condition) {
    // code
} else {
    // code
}

if (condition1) {
    // code
} elif (condition2) {
    // code
} else {
    // code
}
```

### While Loop

```javascript
var i = 0;
while (i < 10) {
    print(i);
    i += 1;
}
```

### Do-While Loop

```javascript
var i = 0;
do {
    print(i);
    i += 1;
} while (i < 10);
```

### For Loop

```javascript
// Traditional for loop
for (var i = 0; i < 10; i += 1) {
    print(i);
}

// For-in loop (iterate over vector)
for (item in [10, 20, 30]) {
    print(item);
}
```

### Switch Statement

```javascript
switch (status) {
    case 1:
        print("Active");
    case 2:
        print("Pending");
    case 3:
        print("Inactive");
    default:
        print("Unknown");
}
```

### Break and Continue

```javascript
for (var i = 0; i < 10; i += 1) {
    if (i == 5) continue;  // Skip 5
    if (i == 8) break;     // Stop at 8
    print(i);
}
```

---

## Functions

### Function Definition

```javascript
func add(a, b) {
    return a + b;
}

var result = add(5, 3);  // 8
```

### Shorthand Function Definition

```javascript
// Using assignment syntax
square(x) = x * x;
area(w, h) = w * h;

var result = square(5);  // 25
```

### Anonymous Functions

```javascript
var multiply = func(a, b) {
    return a * b;
};

var result = multiply(4, 5);  // 20
```

### Arrow Functions (Lambdas)

```javascript
// Single expression (implicit return)
var double = (x) => x * 2;
var add = (a, b) => a + b;

// No parameters
var getPI = () => 3.14159;

// Block body (explicit return)
var complex = (x) => {
    var temp = x * 2;
    return temp + 1;
};

// Usage
var result = double(5);  // 10
```

### Default Parameters

```javascript
func greet(name = "World", greeting = "Hello") {
    return greeting + ", " + name + "!";
}

greet();                    // "Hello, World!"
greet("Alice");             // "Hello, Alice!"
greet("Bob", "Hi");         // "Hi, Bob!"
```

### Variadic Functions

Functions can accept variable number of arguments:

```javascript
func sum_all(...args) {
    var total = 0;
    for (arg in args) {
        total += arg;
    }
    return total;
}

sum_all(1, 2, 3);        // 6
sum_all(1, 2, 3, 4, 5);  // 15
```

### Recursion

```javascript
func factorial(n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

factorial(5);  // 120

func fibonacci(n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

fibonacci(10);  // 55
```

### Closures

Functions can capture variables from outer scope:

```javascript
func makeCounter() {
    var count = 0;
    return func() {
        count += 1;
        return count;
    };
}

var counter = makeCounter();
counter();  // 1
counter();  // 2
counter();  // 3
```

---

## Collections

### Vector Operations

```javascript
var v = [10, 20, 30, 40, 50];

// Indexing (0-based)
v[0]       // 10
v[4]       // 50

// Slicing (exclusive end)
v[1..4]    // [20, 30, 40]
v[0..2]    // [10, 20]

// Length
len(v)     // 5
v.length() // 5 (dot-style)

// Aggregation
sum(v)     // 150
avg(v)     // 30
min(v)     // 10
max(v)     // 50

// Dot-style (equivalent)
v.sum()    // 150
v.avg()    // 30
v.min()    // 10
v.max()    // 50

// Mutating operations
v.push(60);        // v = [10, 20, 30, 40, 50, 60]
var last = v.pop(); // last = 60, v = [10, 20, 30, 40, 50]

// Sorting
var sorted = sort(v);  // Returns sorted copy
v.sort();              // Returns sorted copy

// Search
v.indexOf(30);         // 2
vector_find_value(v, 30);     // 2 (first occurrence)
vector_find_all(v, 30);       // [2] (all occurrences)

// Reverse
v.reverse();           // Returns reversed copy
```

### Vector Arithmetic

Element-wise operations with scalar broadcasting:

```javascript
[1, 2, 3] + [4, 5, 6]  // [5, 7, 9]
[1, 2, 3] * 10          // [10, 20, 30]
[10, 20] - [3, 4]       // [7, 16]
[2, 4, 6] / 2           // [1, 2, 3]
```

### String Operations

```javascript
var s = "Hello World";

// Case conversion
s.lower()              // "hello world"
s.upper()              // "HELLO WORLD"

// Trimming
"  hello  ".trim()     // "hello"
"  hello".ltrim()      // "hello"
"hello  ".rtrim()      // "hello"

// Searching
s.contains("World")    // 1 (true)
s.indexOf("World")     // 6
s.starts_with("Hello") // 1 (true)
s.ends_with("World")   // 1 (true)

// Substring
s.substr(0, 5)         // "Hello"
s[0..5]                // "Hello" (slicing)

// Replacement
s.replace("World", "TurboScript")  // "Hello TurboScript"

// Length
s.length()             // 11
len(s)                 // 11

// Reverse
s.reverse()            // "dlroW olleH"

// Split
"a,b,c".split(",")     // ["a", "b", "c"] (returns vector)
```

### Map Operations

```javascript
var m = map{name: "Alice", age: 30, city: "NYC"};

// Access
m.name                 // "Alice"
m["age"]               // 30

// Check existence
m.has("city")          // 1 (true)
m.has("country")       // 0 (false)

// Get keys
m.keys()               // ["name", "age", "city"]

// Size
m.size()               // 3

// Delete
m.delete("city");      // Removes "city" key
```

Current contract:

- `m.name` and `m["name"]` both read the same key.
- A missing map key currently evaluates to numeric `0`, not `null`.
- `m.keys()` and `m.values()` return `list`, not `vector`.
- In script condition position, maps do not become truthy; use `m.size()`, `m.has(key)`, or an explicit numeric test.
- `for (k in m)` iterates keys, not values.

### List Operations

```javascript
var lst = list("hello", 42, [1, 2, 3]);

// Indexing
lst[0]                 // "hello"
lst[1]                 // 42
lst[2]                 // [1, 2, 3]

// Length
len(lst)               // 3
lst.length()           // 3
```

Current contract:

- `list` is heterogeneous storage; it is not the default numeric pipeline container.
- `lst[i]` returns the original value shape at that index.
- In script condition position, lists do not become truthy; use `lst.length()` or another explicit numeric test.

### Container Contract

TurboScript currently has three different collection roles:

- `vector`: dense numeric data. Use it for math, slicing, TA, and time-series work.
- `map`: named fields and records. Missing keys currently fall back to numeric `0`.
- `list`: heterogeneous transport. Use it when values are mixed-type or when APIs return keys/values.

Indexing and slicing rules:

- `vector[i]` returns a number and throws on out-of-bounds access.
- `vector[a..b]` returns a sliced vector with exclusive end.
- `map[key]` requires a string key and returns the stored value, or numeric `0` when absent.
- `list[i]` returns the stored value and throws on out-of-bounds access.

Truthiness rules:

- In script conditions, numbers use numeric truth, strings and bytes use `len > 0`, and containers are true when non-empty.
- `null` is false. Functions, classes, and instances are true.
- Host C APIs use the same broad truthiness model for exported `exprtk_value_t` values.

---

## Object-Oriented Programming

TurboScript supports class-based OOP with runtime class values:

```javascript
class Counter {
    value = 0;

    constructor(start) {
        this.value = start;
    }

    add(delta) {
        this.value = this.value + delta;
        return this.value;
    }

    static zero() {
        return Counter(0);
    }
}

var c = Counter(40);
var result = c.add(2);      // 42
```

`class` and `interface` are bare declarations, not `var` / `let` declarations. Classes are first-class values. Assigning `Alias = Counter` creates another reference to the same class object, so static fields are shared and `Alias(...)`, `new Alias(...)`, and `instanceof Alias` use the alias value.

Inheritance uses `extends`, `super(...)`, and `super.method(...)`:

```javascript
class Base {
    constructor(value) { this.value = value; }
    get() { return this.value; }
}

class Child extends Base {
    constructor(value) { super(value + 1); }
    get() { return super.get() + 1; }
}
```

Abstract classes are supported:

```javascript
abstract class Shape {
    abstract area();
}

class Square extends Shape {
    constructor(size) { this.size = size; }
    area() { return this.size * this.size; }
}
```

Interfaces are supported, including multiple `implements` clauses and interface inheritance:

```javascript
interface Shape {
    area();
}

interface NamedShape extends Shape {
    label();
}

class Square implements NamedShape {
    size = 0;

    constructor(size) { this.size = size; }
    area() { return this.size * this.size; }
    label() { return "square"; }
}
```

Interfaces may also require static methods. The implementing class may provide the static method directly or inherit it from a parent class.

Class bodies may declare instance and static fields. A field without an initializer starts as `null`. Access defaults to `public`; `protected` permits the declaring class and subclasses; `private` permits only the declaring class.

```javascript
class Counter {
    private value = 0;
    protected static seed = 10;

    add(delta) {
        this.value = this.value + delta;
        return this.value;
    }

    static base() {
        return Counter.seed;
    }
}
```

The legacy `_` prefix privacy convention remains active for dynamically created fields and methods. Class methods may access their own `_field` or `_method`, including private static members through the class name or an alias that points to the same class. External access such as `obj._value`, `obj._value()`, `Class._value`, or `Class._value()` is rejected at runtime.

Methods may be overloaded by arity and, for declared class/interface parameter types, by runtime argument type. Direct instance, static, and `super.method(...)` calls select the matching overload:

```javascript
class Counter {
    value() { return 1; }
    value(x) { return x + 1; }
    value(x, y) { return x + y + 1; }
}

var c = Counter();
var total = c.value() + c.value(10) + c.value(10, 20); // 43
```

Current OOP contract:

- Property-style getter/setter syntax is intentionally not supported; use ordinary `get()` / `set(value)` methods.
- Type-based overload resolution is limited to declared class and interface parameter types.
- Non-variable receiver assignment such as `Factory().make().value = 1` is intentionally not supported in JIT.
- JIT/MIR lowers OOP operations through runtime helpers with monomorphic method helpers, callsite method caches, and instance field slot caches. Unsupported non-OOP nodes are rejected rather than silently falling back to the interpreter.

---

## Modules and Imports

### Loading Modules

```javascript
// Load plugin modules
import("csv");
import("json");
import("ta");
import("vec");
import("net");
import("sqlite");
import("fin");
import("mapper");
```

### Module Namespaces

Functions are accessed via module prefix:

```javascript
import("csv");

var data = csv.read("data.csv");
var column = csv.col(data, "price");
var filtered = csv.filter(data, "price > 100");
```

### Script Module Exports

Script imports now support an explicit export surface:

```javascript
// math_utils.tbs
func add1(x) { return x + 1; };
export("add1");
export("answer", 41);

// main.tbs
var mod = import("./math_utils.tbs");
var f = mod.add1;
var result = f(4) + mod.answer;   // 46
```

Current contract:

- `import("./file.tbs")` executes the script once.
- Script import paths must end with `.tbs`; `.ts` is not recognized as a TurboScript source extension.
- If the script calls `export(...)`, `import()` returns a `map` of exported names.
- Re-importing the same resolved script path uses the cached module result and does not re-execute the file.
- Scripts that rely on global side effects still work, but new scripts should prefer explicit exports.

### Isolated Script Modules

When you want module exports without global leakage, use `import_module()`:

```javascript
// scoped_math.tbs
shared = 99;
secret = 5;
func add_secret(x) { return x + secret; };
export("add_secret");
export("shared");

// caller.tbs
shared = 7;
var mod = import_module("./scoped_math.tbs");
var out = mod.add_secret(3);   // 8
var keep = shared;             // still 7
```

Current contract:

- `import_module("./file.tbs")` only accepts script paths.
- Module script paths must end with `.tbs`.
- The imported script runs against an isolated variable snapshot, not the caller's global scope.
- Explicit exports remain available through the returned `map`.
- Re-importing the same resolved script path reuses the cached isolated module result.
- Use plain `import()` only when shared global side effects are required.

### Built-in Functions (No Import Needed)

These are globally available:

```javascript
// Math
sin(x), cos(x), sqrt(x), abs(x), log(x)

// String
lower(s), upper(s), trim(s), substr(s, start, len)

// Vector
sum(v), avg(v), min(v), max(v), len(v)

// File I/O
read_file(path), write_file(path, content), copy_file(src, dst)
file_exists(path), file_size(path)
file_truncate(path, length)
listdir(path), glob(pattern), mkdir_recursive(path), rmdir_recursive(path)

// Date/Time
now(), date(str), date_utc(str)
format_date(timestamp), format_date_utc(timestamp)

// Platform
os_name(), pid(), uptime_ms(), monotonic_ms()
```

File and path built-ins use TurboNet `turbo_fs`. Date formatting and local/UTC
time conversion use TurboNet platform datetime helpers. Native temporal values
are available through `datetime.parse()`, `date.parse()`, `time.parse()`, and
`duration.parse()`.

### Importing Scripts

Load other TurboScript files:

```javascript
// utils.tbs
func helper(x) {
    return x * 2;
}

// main.tbs
import("utils.tbs");
var result = helper(5);  // 10
```

---

## Advanced Features

### Member Call Dispatch

Dot-style method calls are transformed internally:

```javascript
// User writes:
prices.sma(20)

// Internally transformed to:
sma(prices, 20)
```

This works for:
- **Vectors**: `v.sum()`, `v.avg()`, `v.push(x)`
- **Strings**: `s.upper()`, `s.trim()`, `s.substr(0, 5)`
- **Maps**: `m.keys()`, `m.has(key)`, `m.delete(key)`
- **Module functions**: `prices.sma(20)` → `ta.sma(prices, 20)`

### Template Strings

```javascript
var name = "Alice";
var age = 30;
var message = `Hello, ${name}! You are ${age} years old.`;
```

### Mustache Template Rendering

Use `template_render(template, data [, partials])` when the template text should
be driven by map/list/vector data instead of direct expression interpolation.
`mustache_render` is an alias.

```javascript
var data = map{
    name: "Ada",
    html: "<b>",
    items: list(map{name: "one"}, map{name: "two"})
};

var out = template_render(
    "Hi {{name}} {{html}} {{{html}}} {{#items}}{{name}};{{/items}}",
    data
);
// "Hi Ada &lt;b&gt; <b> one;two;"
```

Maps support dotted lookup (`{{user.city}}`), lists and vectors support sections,
and missing values render as empty text. A third `partials` map supplies
`{{>name}}` templates, including dotted partial names such as `{{>layout.item}}`.
Function values in the data map are Mustache lambdas: they receive the raw
section body and return template text that is rendered in the current context.

### Range Operator

```javascript
// Create range (not yet implemented as literal, use vec.range)
import("vec");
var range = vec.range(0, 10);  // [0, 1, 2, ..., 9]
```

---

## Error Handling

### Try-Catch

```javascript
try {
    var result = risky_operation();
    if (result < 0) {
        throw "Negative result not allowed";
    }
} catch (error) {
    print("Error: " + error);
}
```

### Assertions

```javascript
func divide(a, b) {
    assert(b != 0, "Division by zero");
    return a / b;
}

divide(10, 0);  // Aborts with message: "Division by zero"
```

---

## Safety and Limits

TurboScript enforces resource limits to prevent runaway scripts:

### Recursion Limit

```javascript
// Default: 1000 levels
func infinite() {
    return infinite();  // Error after 1000 calls
}
```

### Loop Iteration Limit

```javascript
// Default: 1,000,000 iterations
var i = 0;
while (true) {
    i += 1;  // Error after 1,000,000 iterations
}
```

### Configuring Limits (C API)

```c
turbo_script_ctx_t *ctx = turbo_script_init();
ctx->env->max_recursion = 500;
ctx->env->max_loop_iterations = 100000;
```

---

## Comments

```javascript
// Single-line comment

/*
 * Multi-line comment
 * Can span multiple lines
 */

var x = 10;  // Inline comment
```

---

## Semicolons

Semicolons are **optional** in most cases:

```javascript
// With semicolons
var x = 10;
var y = 20;

// Without semicolons (also valid)
var x = 10
var y = 20

// After blocks, semicolons are optional
func test() {
    return 42;
}  // No semicolon needed
```

---

## Best Practices

### 1. Use Descriptive Names

```javascript
// Good
var closing_prices = [100, 102, 104];
var moving_average = ta.sma(closing_prices, 20);

// Bad
var x = [100, 102, 104];
var y = ta.sma(x, 20);
```

### 2. Prefer Dot-Style for Readability

```javascript
// Good
var total = prices.sum();
var average = prices.avg();

// Also fine
var total = sum(prices);
var average = avg(prices);
```

### 3. Use Pipe Operator for Chains

```javascript
// Good
var result = data
    |> filter(x => x > 0)
    |> map(x => x * 2)
    |> sum();

// Harder to read
var result = sum(map(filter(data, x => x > 0), x => x * 2));
```

### 3.1 Vector Pipeline Primitives

TurboScript now ships a small, explicit pipeline core for numeric vectors:

```javascript
var cleaned = [1, -2, 3, 0]
    |> filter(x => x > 0)
    |> map(x => x * 10);

var total = [1, 2, 3, 4] |> reduce(0, (acc, x) => acc + x);
var head = take([5, 6, 7], 2);      // [5, 6]
var tail = drop([5, 6, 7], 1);      // [6, 7]
var prev = lag([10, 20, 30], 1, -1); // [-1, 10, 20]
```

- `map(vector, fn)` returns a new numeric `vector`.
- `filter(vector, fn)` keeps items whose predicate result is non-zero.
- `reduce(vector, init, fn)` folds left and returns the accumulator.
- `take(vector, n)` and `drop(vector, n)` slice by count.
- `lag(vector, shift, fill=0)` shifts right and fills the front.
- In pipe form, the left side becomes the first argument: `data |> map(f)` means `map(data, f)`.
- This first cut is intentionally `vector`-only. Generic `list` and `map` pipeline semantics are not defined yet.
- Because `map` is also the record-literal keyword, the parser has a dedicated function-call path for `map(...)` and `.map(...)`.

### 3.2 Java Stream-Style Chains

Containers and file sources can also use dot-chain stream calls:

```javascript
var total = stream.csv("trades.csv")
    .filterExpr("price > 100")
    .map(r => to_num(r.price) * to_num(r.qty))
    .reduce(0, (acc, v) => acc + v);

var qty = stream.json("orders.json", "$.orders[*]")
    .filter(r => r.price > 5)
    .map(r => r.qty)
    .reduce(0, (acc, v) => acc + v);

var xml_total = stream.xml("orders.xml", "//price")
    .filter(n => to_num(n.text) > 5)
    .map(n => to_num(n.text))
    .reduce(0, (acc, v) => acc + v);
```

- `stream.of(x)`, `list.stream()`, `vector.stream()`, `map.stream()`, and `string.stream()` create stream values.
- File helpers are `stream.lines(path)`, `stream.csv(path[, has_header])`, `stream.json(path[, jsonpath])`, and `stream.xml(path, xpath)`.
- Chain methods include `filter(fn)`, `filterExpr(expr)`, `where(expr)`, `map(fn)`, `reduce(init, fn)`, `collect()`, `toList()`, `toVector()`, `count()`, and `forEach(fn)`.
- `filterExpr` / `where` use the CSV filter expression syntax and are intended for `stream.csv(...)`.
- `stream.json` uses JSONPath when its second argument is provided, for example `$.orders[*]` or `$.orders[@.price > 5]`.
- `stream.xml` uses XPath 1.0 and yields node maps with `type`, `name`, `text`, and `xml` fields.
- Current file streams are eagerly materialized into runtime values; they are not incremental backpressure streams yet.

### 4. Handle Errors Explicitly

```javascript
// Good
try {
    var data = read_file("data.txt");
    process(data);
} catch (e) {
    print("Failed to read file: " + e);
}

// Risky
var data = read_file("data.txt");  // May fail silently
```

---

## Next Steps

- **[API Reference](api-reference.md)** - Complete function reference
- **[Plugin Development](plugin-development.md)** - Extend TurboScript with C/C++
- **[Module Documentation](modules/)** - Domain-specific guides

---

**Happy scripting!**
