# Mapper Module (`mapper`)

`mapper` provides Jackson-style class-first serialization for JSON, YAML, and
XML. A TurboScript class is the only schema source:

```turboscript
class User {
    name: string;
    age: int64;
    active: bool = true;
}

var user = mapper.read_json(User, "{\"name\":\"Ada\",\"age\":37}");
var json = mapper.write_json(user);
var yaml = mapper.write_yaml(user);
var xml = mapper.write_xml(user);
```

The mapper uses TurboUtils `turbo_parser.h` DOM APIs. It rejects unknown input
fields and values that do not match a declared field type. Missing fields retain
their class defaults. There is no external schema, codec, or binary format.

Supported field types are `string`, `int`/`int64`, `number`, `bool`, `map`,
`object`, `list`, and another TurboScript class name. XML maps scalar fields to
child elements and uses the class name as the root element. JSON and YAML reject
unknown fields; XML currently reads the declared child elements only.
