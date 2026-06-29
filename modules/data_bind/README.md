# Data Bind Module (`data_bind`)

The `data_bind` plugin enables high-performance binary-to-object parsing using JIT-compiled TBE schemas directly inside TurboScript.

## Functions

### `data_bind.create(schema_path)`
Loads a TBE schema file and compiles it into MIR JIT-executable parsing functions.
* **Arguments**: `schema_path` (string)
* **Returns**: `handle` (number >= 0 on success, or `-1` on failure)

### `data_bind.parse(handle, type_name, bytes)`
Parses a binary payload (given as a raw byte string) into a dynamic nested object/list tree representation in the environment.
* **Arguments**:
  * `handle` (number) - The schema codec handle returned by `create`.
  * `type_name` (string) - The message type to parse from the schema.
  * `bytes` (string) - The raw binary buffer. The string contents are passed directly to the JIT parser.
* **Returns**: `object` (plain object on success, or `0` on failure)

Returned records are TurboScript plain objects: `typeof(value)` returns
`"object"` and `is_object(value)` returns `1`. They support dot access,
string index access, for-in iteration, spread, and JSON serialization. TBE
`map<K,V>` fields remain script `map` values inside the object tree because
they represent schema map fields, not record objects.

Scalar fields preserve native TurboScript runtime types where available:
schema `bool` maps to `bool`, `int64/uint64` maps to `int64`, fixed/variable
`bytes` maps to `bytes`, schema `uuid` maps to native `uuid`, schema
`datetime` maps to native `datetime` for JSON/CSV/XML text binding, and other
numeric fields map to `number`.

For text binding, JSON/XML bytes fields read string text as the byte sequence and
CSV bytes fields read the cell text. Binary parsing reads fixed or variable TBE
bytes payloads directly.

### `data_bind.close(handle)`
Frees the underlying schema JIT module and resources.
* **Arguments**: `handle` (number)
* **Returns**: `0`

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

// 2. Prepare raw bytes for the payload.
// Example layout: count (32-bit LE) followed by key-value pairs.
var payload = hex_decode("0200000001000000781e000000010000007928000000");

// 3. Parse bytes to a structured plain object
var msg = data_bind.parse(handle, "Attrs", payload);
if (msg == 0) {
    print("Parse error");
} else {
    print("Parsed attrs.x: " + msg.attrs.x); // Prints 30
    print("Parsed attrs.y: " + msg.attrs.y); // Prints 40
}

// 4. Release resources
data_bind.close(handle);
```
