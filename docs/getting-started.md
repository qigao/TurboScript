# Getting Started with TurboScript

This guide will help you write and run your first TurboScript program in 5 minutes.

TurboScript is a TypeScript/JavaScript-like embeddable scripting engine. It lowers scripts to MIR and can execute the same IR through MIR's interpreter or MIR-generated native code. Use parser/data-bind plus math, matrix, time-series, TA, and finance modules for lightweight data analysis inside host applications.

---

## Hello World

Create a file `hello.tbs`:

```javascript
var name = "World";
print("Hello, " + name + "!");
```

Run it:
```bash
turbo_script hello.tbs
```

Output:
```
Hello, World!
```

---

## Basic Syntax

### Variables and Types

```javascript
// Numbers
var age = 25;
var pi = 3.14159;

// Strings
var greeting = "hello";
var message = 'world';

// Vectors (numeric arrays)
var prices = [100, 102, 104, 103, 105];

// Maps (key-value pairs)
var user = map{name: "Alice", age: 30};

// Null values
var empty = null;  // or nil
```

### Operators

```javascript
// Arithmetic
var x = 10 + 5;      // 15
var y = x * 2;       // 30
var z = y / 3;       // 10

// Comparison
var isEqual = (x == 15);        // 1 (true)
var isGreater = (y > x);        // 1 (true)

// Logical
var result = (x > 5) and (y < 50);   // 1 (true)
var result2 = (x > 5) && (y < 50);   // same as above
```

### Control Flow

```javascript
// If-else
if (age >= 18) {
    print("Adult");
} else {
    print("Minor");
}

// Ternary operator
var status = age >= 18 ? "Adult" : "Minor";

// Loops
for (var i = 0; i < 5; i += 1) {
    print(i);
}

// For-in loop
for (price in prices) {
    print(price);
}
```

### Functions

```javascript
// Function definition
func square(x) {
    return x * x;
}

var result = square(5);  // 25

// Arrow functions (lambdas)
var double = (x) => x * 2;
var result2 = double(10);  // 20

// Default parameters
func greet(name = "World") {
    return "Hello, " + name;
}

greet();         // "Hello, World"
greet("Alice");  // "Hello, Alice"
```

---

## Working with Data

### Vectors

```javascript
var numbers = [10, 20, 30, 40, 50];

// Indexing (0-based)
var first = numbers[0];      // 10
var last = numbers[4];       // 50

// Slicing
var middle = numbers[1..4];  // [20, 30, 40]

// Built-in operations
var total = sum(numbers);    // 150
var average = avg(numbers);  // 30
var minimum = min(numbers);  // 10
var maximum = max(numbers);  // 50

// Dot-style (equivalent)
var total2 = numbers.sum();     // 150
var average2 = numbers.avg();   // 30
```

### Strings

```javascript
var text = "Hello World";

// String operations
var lower = text.lower();           // "hello world"
var upper = text.upper();           // "HELLO WORLD"
var length = text.length();         // 11
var contains = text.contains("World");  // 1 (true)

// Substring
var sub = text.substr(0, 5);        // "Hello"

// String slicing
var slice = text[0..5];             // "Hello"
```

### Maps

```javascript
var person = map{
    name: "Alice",
    age: 30,
    city: "NYC"
};

// Access values
var name = person.name;              // "Alice"
var age = person["age"];             // 30

// Check existence
var hasCity = person.has("city");    // 1 (true)

// Get keys
var keys = person.keys();            // ["name", "age", "city"]
```

---

## File Operations

TurboScript has built-in file I/O (no import needed):

```javascript
// Write to file
write_file("output.txt", "Hello, File!");

// Read from file
var content = read_file("output.txt");
print(content);  // "Hello, File!"

// Check if file exists
if (file_exists("data.txt")) {
    var data = read_file("data.txt");
    print(data);
}

// File info
var size = file_size("output.txt");  // bytes
var isFile = is_file("output.txt");  // 1 (true)

// Copy and truncate
copy_file("output.txt", "backup.txt");
file_truncate("backup.txt", 5);

// Directory listing and recursive directories
var files = listdir(".");
var scripts = glob("*.tbs");
mkdir_recursive("data/tmp/cache");
rmdir_recursive("data/tmp");
```

Date/time helpers are also built in:

```javascript
var now_ts = now();
var utc_ts = date_utc("2024-01-01T12:00:00Z");
var text = format_date_utc(utc_ts, "%Y-%m-%d %H:%M:%S");
```

---

## Using Modules

TurboScript can be extended with modules (plugins):

```javascript
// Load CSV module
import("csv");

// Read CSV file
var data = csv.read("prices.csv");

// Get column
var close_prices = csv.col(data, "close");

// Process data
var average_price = avg(close_prices);
print("Average price: " + average_price);
```

### Common Modules

```javascript
import("csv");      // CSV parsing
import("json");     // JSON parsing
import("ta");       // Technical analysis
import("vec");      // Advanced vector operations
import("net");      // HTTP/WebSocket
import("sqlite");   // Database access
import("mapper");   // Class-first JSON/YAML/XML mapping
```

---

## Example: Data Analysis

Let's analyze stock prices:

```javascript
import("csv");
import("ta");

// Read data
var data = csv.read("AAPL.csv");
var close = csv.col(data, "close");

// Calculate indicators
var sma20 = ta.sma(close, 20);    // 20-day moving average
var sma50 = ta.sma(close, 50);    // 50-day moving average
var rsi = ta.rsi(close, 14);      // RSI indicator

// Get latest values
var latest_price = close[len(close) - 1];
var latest_rsi = rsi[len(rsi) - 1];

// Generate signal
var signal = "HOLD";
if (latest_rsi < 30) {
    signal = "BUY - Oversold";
} elif (latest_rsi > 70) {
    signal = "SELL - Overbought";
}

print("Price: " + latest_price);
print("RSI: " + latest_rsi);
print("Signal: " + signal);
```

---

## Next Steps

Now that you know the basics:

1. **[Language Guide](language-guide.md)** - Learn advanced features (destructuring, closures, error handling)
2. **[API Reference](api-reference.md)** - Explore all built-in functions
3. **[Plugin Development](plugin-development.md)** - Create your own modules in C/C++

---

## Quick Tips

### Pipe Operator

Chain operations elegantly:

```javascript
var result = data
    |> filter(x => x > 0)
    |> map(x => x * 2)
    |> sum();
```

### Optional Chaining

Safely access nested properties:

```javascript
var city = user?.address?.city;  // Returns null if any part is null
```

### Destructuring

Extract values concisely:

```javascript
var [a, b, c] = [10, 20, 30];
var map{name, age} = person;
```

---

**Ready to dive deeper? Check out the [Language Guide](language-guide.md)!**
