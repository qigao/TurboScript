# Parser Module

`parser` handles configuration-oriented text formats. JSON, YAML, CSV, XML,
Structured class mapping belongs to the `mapper` module. This module remains
limited to configuration-oriented text parsing.

## API

- `parser.ini_get(text, section, key, default)` reads one INI value.
- `parser.dotenv_load(path)` loads variables from a dotenv file.
- `parser.dotenv_load_default()` loads the default dotenv file.
- `parser.toml_parse(text)` parses TOML text into native values.
- `parser.toml_parse_file(path)` parses a TOML file.
- `parser.cmd_parse(args, specification)` parses command-line arguments.

The module intentionally contains no JSON, CSV, XML, TOON, schema-binding, or
structured-file stream implementation. Load `mapper` for typed JSON, YAML, and
XML class mapping.

## Build And Test

```powershell
cmake --build --preset win-dev-user --target parser_tbs test_parser
ctest --preset win-dev-user -R parser_module --output-on-failure
```
