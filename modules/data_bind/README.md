# Data Bind Module (`data_bind`)

The `data_bind` plugin enables high-performance binary-to-object parsing using JIT-compiled TBE schemas directly inside TurboScript.

## Functions

### `data_bind.create(schema_path)`
Loads a TBE schema file and compiles it into MIR JIT-executable parsing functions.
* **Arguments**: `schema_path` (string)
* **Returns**: `handle` (number >= 0 on success, or `-1` on failure)

### `data_bind.parse(handle, type_name, bytes_vec)`
Parses a binary payload (given as a numeric byte vector) into a dynamic nested map/list tree representation in the environment.
* **Arguments**:
  * `handle` (number) - The schema codec handle returned by `create`.
  * `type_name` (string) - The message type to parse from the schema.
  * `bytes_vec` (vector/double[]) - The raw binary buffer carrying byte values (0-255).
* **Returns**: `map` (object map on success, or `0` on failure)

### `data_bind.close(handle)`
Frees the underlying schema JIT module and resources.
* **Arguments**: `handle` (number)
* **Returns**: `0`

### `data_bind.error(handle)`
Retrieves the last error message from the codec.
* **Arguments**: `handle` (number)
* **Returns**: `string`

---

## Example Usage

```javascript
import("data_bind");

// 1. Compile schema
var handle = data_bind.create("message.tbe");
if (handle < 0) {
    print("Failed to compile schema!");
    exit(1);
}

// 2. Prepare byte array (vector) for the payload
// Example layout: count (32-bit LE) followed by key-value pairs
var payload = [
    2, 0, 0, 0,    // count = 2
    1, 0, 0, 0,    // key length = 1
    120,           // 'x'
    30, 0, 0, 0,   // value = 30
    1, 0, 0, 0,    // key length = 1
    121,           // 'y'
    40, 0, 0, 0    // value = 40
];

// 3. Parse bytes to structured map object
var msg = data_bind.parse(handle, "Attrs", payload);
if (msg == 0) {
    print("Parse error: " + data_bind.error(handle));
} else {
    print("Parsed attrs.x: " + msg.attrs.x); // Prints 30
    print("Parsed attrs.y: " + msg.attrs.y); // Prints 40
}

// 4. Release resources
data_bind.close(handle);
```
