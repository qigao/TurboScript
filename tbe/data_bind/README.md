# DataBind Public C API

`tbe/data_bind` is the third-party integration boundary for TurboScript schema
binding. It exposes a C ABI over opaque handles and dynamic value accessors.
Consumers should not depend on `modules/data_bind` or `exprtk` unless they are
embedding TurboScript scripts.

`modules/parser` uses this library for TurboScript-facing schema binding,
strict boolean validation, and schema reflection. Parser remains responsible
for script value conversion, JSON/CSV emit, detailed `validate_ex` diagnostics,
and format-specific query helpers such as JSONPath, XPath, and CSV filters.
Format parsing inside `tbe/data_bind` uses TurboNet::Parser directly; it does
not call into `modules/parser`.

## CMake

After installing TurboScript:

```cmake
find_package(TurboScript CONFIG REQUIRED)

add_executable(app main.c)
target_link_libraries(app PRIVATE TurboScript::DataBind)
```

On Windows, `TurboScript::DataBind` defines `DATA_BIND_USE_DLL` for consumers.
Do not define `DATA_BIND_BUILD_DLL` outside the library itself.

## Minimal Use

```c
#include "data_bind.h"
#include <string.h>

DataBind *codec = NULL;
DataBindError err = DATA_BIND_ERROR_INIT;
DataBindStatus status = data_bind_create("orders.tbe", &codec, &err);
if (status != DATA_BIND_OK) {
  return 1;
}

const char *json = "{\"id\":7,\"symbol\":\"ABCD\"}";
DataBindValue *order = NULL;
status = data_bind_parse_json(codec, "Order", json, strlen(json), &order, &err);
if (status != DATA_BIND_OK) {
  data_bind_free(codec);
  return 1;
}

const DataBindValue *id = data_bind_value_get(order, "id");
int32_t value = 0;
if (data_bind_value_get_int32(id, &value) != DATA_BIND_OK) {
  data_bind_value_free(order);
  data_bind_free(codec);
  return 1;
}

data_bind_value_free(order);
data_bind_free(codec);
```

## Ownership

- `DataBind*` is owned by the caller and released with `data_bind_free`.
- `DataBindValue*` results are owned by the caller and released with
  `data_bind_value_free`.
- `const char*`, child `DataBindValue*`, and map entries returned by accessor
  functions are borrowed. They become invalid when the owning codec or value is
  freed.
- It is valid to pass `NULL` to `data_bind_free` and `data_bind_value_free`.

## Error Handling

The public ABI uses `DataBindStatus` return values plus caller-owned
`DataBindError` storage. Functions that create or parse values return
`DATA_BIND_OK` on success and write the owned result through an out parameter:

```c
DataBindError err = DATA_BIND_ERROR_INIT;
DataBindValue *value = NULL;
DataBindStatus status =
  data_bind_parse_json(codec, "Order", json, strlen(json), &value, &err);
if (status != DATA_BIND_OK) {
  fprintf(stderr, "%s: %s\n", data_bind_status_name(status), err.message);
}
```

`DataBindError` includes `code`, `line`, `column`, `path`, and `message`. The
message buffer belongs to the caller-provided struct, so it is safe to read
after the codec function returns.

## Supported Inputs

- `data_bind_parse`: binary TBE payload.
- `data_bind_parse_json`: JSON object or scalar matching the schema type.
- `data_bind_parse_json_all`: JSON array binds each item; non-array binds as a
  one-item list.
- `data_bind_parse_csv`: CSV with a header row. Nested fields use paths such as
  `header.seq`, `bids[0].price`, and `attrs.x`.
- `data_bind_parse_csv_all`: binds every parsed CSV row.
- `data_bind_parse_xml`: XML document root binds to the requested schema type.
  Record fields bind from same-name child elements first and same-name
  attributes second. Repeated same-name elements bind lists/groups.
- `data_bind_parse_xml_all`: XPath selects nodes and binds each selected node.
  Use expressions supported by the bundled TurboNet XML parser, such as
  `//order`.
- `data_bind_validate_json`: strictly validates JSON. Arrays are valid only
  when every item binds to the requested schema type.
- `data_bind_validate_csv`: strictly validates every CSV data row against the
  requested schema type. CSV uses the same header path rules as CSV binding.
- `data_bind_validate_xml`: strictly validates the document root or every node
  selected by the supplied XPath expression.
- `data_bind_create_from_text`: creates a codec directly from schema text in
  memory.

`parse_*_all` APIs are binding helpers: they return a list of successfully
bound values and may skip invalid array items or CSV rows. Use
`data_bind_validate_json`, `data_bind_validate_csv`, or
`data_bind_validate_xml` when the caller needs strict all-or-nothing input
validation.

CSV and XML use the same schema binder as JSON:

- CSV nested columns use paths such as `header.seq`, `levels[0]`,
  `bids[0].price`, and `attrs.x`; sanitized names such as `header_seq` are
  also accepted by the TurboScript parser adapter.
- CSV top-level scalar/enum/flags values use a `value` column, with single
  column CSV accepted by binding and validation.
- CSV union values choose the variant from non-empty payload columns on each
  row, so merged dynamic headers can represent different variants per row.
- XML binds record fields from same-name child elements first and attributes
  second. `data_bind_parse_xml_all` and `data_bind_validate_xml` accept XPath
  to select repeated record nodes.

## Strict Value Access

Convenience accessors such as `data_bind_value_as_int()` remain available, but
they coerce or return zero on invalid input. New third-party code should prefer
strict accessors:

- `data_bind_value_get_int32`
- `data_bind_value_get_int64`
- `data_bind_value_get_double`
- `data_bind_value_get_bool`
- `data_bind_value_get_string`
- `data_bind_value_get_bytes`
- `data_bind_value_get_uuid`
- `data_bind_value_get_datetime`
- `data_bind_value_get_date`
- `data_bind_value_get_time`
- `data_bind_value_get_duration_milliseconds`
- `data_bind_value_get_decimal`
- `data_bind_value_get_bigint`
- `data_bind_value_get_money`

Strict accessors return `DATA_BIND_OK` only when the requested conversion is
valid.

Schema `bool` values bind to `DATA_BIND_VALUE_BOOL`. Numeric convenience
accessors still read them as `0` or `1`, but strict type checks should use
`data_bind_value_get_bool`.

Schema `uuid` values bind to `DATA_BIND_VALUE_UUID`. Binary parsing reads the
fixed 16-byte field payload. JSON, CSV, and XML binding accept canonical UUID
text such as `01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001`; use
`data_bind_value_get_uuid` or `data_bind_value_as_uuid_string` to read it.

Schema `bytes` values bind to `DATA_BIND_VALUE_BYTES`. Binary parsing reads
fixed or variable bytes payloads, while JSON/XML text binding reads string
content and CSV binding reads the cell text as bytes. Use
`data_bind_value_get_bytes` or `data_bind_value_as_bytes` to read the borrowed
byte view.

Schema `datetime` values bind to `DATA_BIND_VALUE_DATETIME` for JSON, CSV, and
XML text binding. Accepted text is parsed directly through TurboNet::Parser, and
callers can read the native `turbo_datetime_t` with `data_bind_value_get_datetime`
or format it with `data_bind_value_as_datetime_string`. Binary parsing does not
define an implicit datetime wire format; use an explicit numeric schema field
for binary timestamps.

Schema `date`, `time`, and `duration` values bind to `DATA_BIND_VALUE_DATE`,
`DATA_BIND_VALUE_TIME`, and `DATA_BIND_VALUE_DURATION` for JSON, CSV, and XML
text binding. `date` accepts `YYYY-MM-DD`, `YYYY/MM/DD`, or datetime text from
which the date can be extracted. `time` accepts `HH:MM`, `HH:MM:SS`, or
`HH:MM:SS.mmm`. `duration` accepts unit text such as `1h30m5s250ms` and emitted
`H:MM:SS.mmm` text. Use the matching strict accessors or string format helpers
to read them. Binary parsing does not define implicit wire formats for these
temporal scalars.

Schema `decimal` values bind to `DATA_BIND_VALUE_DECIMAL` for JSON, CSV, and
XML text binding. The public representation is `DataBindDecimal { int64_t
mantissa; int32_t scale; }`, where `123.45` is stored as `mantissa=12345` and
`scale=2`. Parsing normalizes trailing fractional zeroes, and
`data_bind_value_as_decimal_string` emits the normalized decimal text. Schema
JSON emit uses strings so decimal precision is not forced through binary
floating point. Binary parsing does not define an implicit decimal wire format.

Schema `bigint` values bind to `DATA_BIND_VALUE_BIGINT` for JSON, CSV, and XML
text binding. The public representation is an owned canonical decimal string,
so values larger than `int64` keep exact precision. JSON string input is the
full-precision path; JSON number input is accepted only for safe integer-sized
values. Use `data_bind_value_get_bigint` or `data_bind_value_as_bigint_string`
to read the borrowed text.

Schema `money` values bind to `DATA_BIND_VALUE_MONEY` for JSON, CSV, and XML
text binding. The public representation is `DataBindMoney { DataBindDecimal
amount; char currency[4]; }`. Text accepts `USD 123.45` and `123.45 USD`;
JSON also accepts `{ "amount": "123.45", "currency": "USD" }`. Use
`data_bind_value_get_money` or `data_bind_value_as_money_string`.

String fields can declare a validation format with field attributes:

```tbe
message Endpoint {
  [format(ipaddr)] string ip;
  [format(url)] string href;
  [format(email)] string owner;
}
```

Formats validate JSON, CSV, XML, and default text during binding and
validation, but the bound value remains `DATA_BIND_VALUE_STRING`. Supported
formats are `ipaddr`, `ip`, `cidr`, `hostname`, `domain`, `email`, `url`,
`uri`, `macaddr`, `mac`, `semver`, `hex`, `base64`, `base64url`, and
`currency`, `json_pointer`, `jsonpath`, `xpath`, `cron`, `color`, `mime`, and
`regex`. Regex format validation compiles the pattern with libfsm/libre.
Unknown formats are ignored so external schema annotations can coexist with
DataBind.

## Schema Reflection

Reflection outputs are caller-owned structs. Initialize them with the provided
macros so the library can honor the struct size across ABI revisions:

```c
DataBindSchemaType type = DATA_BIND_SCHEMA_TYPE_INIT;
if (data_bind_schema_find_type(codec, "Order", &type)) {
  printf("%s has %zu fields\n", type.name, type.field_count);
}

DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
if (data_bind_schema_field_at(codec, "Order", 0, &field)) {
  printf("%s: %s\n", field.name, field.kind);
  if (field.format) printf(" format=%s\n", field.format);
}
```

The TurboScript parser module delegates `schema.types`, `schema.fields`,
`schema.type_exists`, `schema.enums`, `schema.flags`, and `schema.unions` to
these reflection APIs. Parser-local schema attributes/layout helpers currently
remain outside this ABI because they preserve parser-specific layout summaries.

## MIR Output

`data_bind_generate_mir` writes through a callback instead of exposing `FILE*`
across the ABI:

```c
static int write_cb(const void *data, size_t len, void *user) {
  FILE *out = (FILE *)user;
  return fwrite(data, 1, len, out) == len ? 0 : -1;
}

DataBindError err = DATA_BIND_ERROR_INIT;
DataBindStatus status =
  data_bind_generate_mir("orders.tbe", write_cb, stdout, 0, &err);
```

## Version Checks

`DATA_BIND_VERSION` is the compile-time header version. At runtime:

```c
if (data_bind_abi_version() != DATA_BIND_ABI_VERSION) {
  /* Header/library ABI mismatch. */
}
```

`data_bind_library_version()` returns `major * 10000 + minor * 100 + patch`.
`data_bind_version_string()` returns a diagnostic string.
