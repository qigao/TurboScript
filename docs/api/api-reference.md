# TurboScript API Reference

Complete reference for all built-in functions and standard library.

---

## Table of Contents

1. [Mathematical Functions](#mathematical-functions)
2. [Statistical Functions](#statistical-functions)
3. [String Functions](#string-functions)
4. [Vector Functions](#vector-functions)
5. [Linear Algebra](#linear-algebra)
6. [Tabular Data Helpers](#tabular-data-helpers)
7. [File I/O](#file-io)
8. [Date and Time](#date-and-time)
9. [Type Conversion](#type-conversion)
10. [Platform Functions](#platform-functions)

For module-specific functions (CSV, JSON, TA, etc.), see [Module Documentation](modules/).

---

## Mathematical Functions

### Basic Math

| Function | Description | Example |
|----------|-------------|---------|
| `abs(x)` | Absolute value | `abs(-5)` → `5` |
| `sqrt(x)` | Square root | `sqrt(16)` → `4` |
| `cbrt(x)` | Cube root | `cbrt(27)` → `3` |
| `pow(x, y)` | Power (x^y) | `pow(2, 3)` → `8` |
| `exp(x)` | e^x | `exp(1)` → `2.718...` |
| `exp2(x)` | 2^x | `exp2(3)` → `8` |
| `expm1(x)` | e^x - 1 (accurate for small x) | `expm1(0.001)` |
| `log(x)` | Natural logarithm | `log(e)` → `1` |
| `log2(x)` | Base-2 logarithm | `log2(8)` → `3` |
| `log10(x)` | Base-10 logarithm | `log10(100)` → `2` |
| `log1p(x)` | log(1 + x) (accurate for small x) | `log1p(0.001)` |
| `logn(x, base)` | Logarithm with arbitrary base | `logn(8, 2)` → `3` |

### Trigonometry

| Function | Description | Example |
|----------|-------------|---------|
| `sin(x)` | Sine (radians) | `sin(pi/2)` → `1` |
| `cos(x)` | Cosine (radians) | `cos(0)` → `1` |
| `tan(x)` | Tangent (radians) | `tan(pi/4)` → `1` |
| `asin(x)` | Arc sine | `asin(1)` → `pi/2` |
| `acos(x)` | Arc cosine | `acos(1)` → `0` |
| `atan(x)` | Arc tangent | `atan(1)` → `pi/4` |
| `atan2(y, x)` | Arc tangent of y/x | `atan2(1, 1)` → `pi/4` |
| `sinh(x)` | Hyperbolic sine | `sinh(0)` → `0` |
| `cosh(x)` | Hyperbolic cosine | `cosh(0)` → `1` |
| `tanh(x)` | Hyperbolic tangent | `tanh(0)` → `0` |

### Rounding

| Function | Description | Example |
|----------|-------------|---------|
| `ceil(x)` | Round up | `ceil(3.2)` → `4` |
| `floor(x)` | Round down | `floor(3.8)` → `3` |
| `round(x)` | Round to nearest | `round(3.5)` → `4` |
| `trunc(x)` | Truncate to integer | `trunc(3.9)` → `3` |
| `fract(x)` | Fractional part | `fract(3.7)` → `0.7` |

### Utility

| Function | Description | Example |
|----------|-------------|---------|
| `mod(a, b)` | Modulo | `mod(7, 3)` → `1` |
| `sgn(x)` | Sign (-1, 0, or 1) | `sgn(-5)` → `-1` |
| `copysign(x, y)` | Magnitude of x with sign of y | `copysign(5, -1)` → `-5` |
| `hypot(x, y)` | sqrt(x² + y²) | `hypot(3, 4)` → `5` |
| `clamp(x, lo, hi)` | Clamp x to [lo, hi] | `clamp(15, 0, 10)` → `10` |
| `saturate(x)` | Clamp x to [0, 1] | `saturate(1.5)` → `1` |
| `step(edge, x)` | 0 if x < edge, else 1 | `step(5, 3)` → `0` |

### Interpolation

| Function | Description | Example |
|----------|-------------|---------|
| `lerp(a, b, t)` | Linear interpolation | `lerp(0, 10, 0.5)` → `5` |
| `inverse_lerp(a, b, x)` | Inverse lerp (find t) | `inverse_lerp(0, 10, 5)` → `0.5` |
| `remap(x, in0, in1, out0, out1)` | Remap range | `remap(5, 0, 10, 0, 100)` → `50` |
| `smoothstep(e0, e1, x)` | Hermite interpolation | `smoothstep(0, 1, 0.5)` → `0.5` |

### Angle Conversion

| Function | Description | Example |
|----------|-------------|---------|
| `radians(deg)` | Degrees to radians | `radians(180)` → `pi` |
| `degrees(rad)` | Radians to degrees | `degrees(pi)` → `180` |

### Predicates

| Function | Description | Example |
|----------|-------------|---------|
| `is_nan(x)` | Check if NaN | `is_nan(0/0)` → `1` |
| `is_inf(x)` | Check if infinite | `is_inf(1/0)` → `1` |

### Machine Learning Activations

| Function | Description | Example |
|----------|-------------|---------|
| `relu(x)` | max(0, x) | `relu(-5)` → `0` |
| `sigmoid(x)` | 1 / (1 + e^-x) | `sigmoid(0)` → `0.5` |
| `softplus(x)` | log(1 + e^x) | `softplus(0)` → `0.693` |

### Number Theory

| Function | Description | Example |
|----------|-------------|---------|
| `fibonacci(n)` | n-th Fibonacci number | `fibonacci(10)` → `55` |
| `gcd(a, b)` | Greatest common divisor | `gcd(12, 8)` → `4` |

### Random

| Function | Description | Example |
|----------|-------------|---------|
| `rand()` | Random [0, 1) | `rand()` → `0.742...` |
| `normal_rand(mu, sigma)` | Normal distribution | `normal_rand(0, 1)` |

---

## Statistical Functions

### Aggregation

| Function | Description | Example |
|----------|-------------|---------|
| `sum(v)` | Sum of elements | `sum([1,2,3])` → `6` |
| `avg(v)` | Average (mean) | `avg([1,2,3])` → `2` |
| `min(v)` | Minimum value | `min([3,1,2])` → `1` |
| `max(v)` | Maximum value | `max([3,1,2])` → `3` |
| `len(v)` | Number of elements | `len([1,2,3])` → `3` |

### Distribution

| Function | Description | Example |
|----------|-------------|---------|
| `median(v)` | Median value | `median([1,2,3,4,5])` → `3` |
| `percentile(v, p)` | p-th percentile (0-100) | `percentile([1,2,3,4,5], 75)` → `4` |
| `geometric_mean(v)` | Geometric mean | `geometric_mean([1,2,4])` → `2` |
| `harmonic_mean(v)` | Harmonic mean | `harmonic_mean([1,2,4])` → `1.714` |
| `stats.normal_pdf(x[, mu, sigma])` | Normal probability density | `stats.normal_pdf(0)` |
| `stats.normal_cdf(x[, mu, sigma])` | Normal cumulative distribution | `stats.normal_cdf(0)` → `0.5` |
| `stats.normal_quantile(p[, mu, sigma])` | Inverse normal CDF | `stats.normal_quantile(0.5)` → `0` |
| `stats.t_pdf(x, df)` | Student t probability density | `stats.t_pdf(0, 10)` |
| `stats.t_cdf(x, df)` | Student t cumulative distribution | `stats.t_cdf(0, 10)` → `0.5` |
| `stats.t_quantile(p, df)` | Inverse Student t CDF | `stats.t_quantile(0.5, 10)` → `0` |
| `stats.t_test_1samp(v, mean)` | One-sample t-test result map: `t`, `df`, `p` | `stats.t_test_1samp([1,2,3], 2)` |

### Moments

| Function | Description | Example |
|----------|-------------|---------|
| `skewness(v)` | Sample skewness | `skewness([1,2,3,4,5])` |
| `kurtosis(v)` | Sample excess kurtosis | `kurtosis([1,2,3,4,5])` |

### Transformations

| Function | Description | Example |
|----------|-------------|---------|
| `cumsum(v)` | Cumulative sum | `cumsum([1,2,3])` → `[1,3,6]` |
| `rank(v)` | Rank values | `rank([30,10,20])` → `[3,1,2]` |
| `zscore(v)` | Z-score normalization | `zscore([1,2,3,4,5])` |

### Sorting

| Function | Description | Example |
|----------|-------------|---------|
| `sort(v)` | Sort ascending | `sort([3,1,2])` → `[1,2,3]` |

---

## String Functions

### Case Conversion

| Function | Description | Example |
|----------|-------------|---------|
| `lower(s)` | Convert to lowercase | `lower("HELLO")` → `"hello"` |
| `upper(s)` | Convert to uppercase | `upper("hello")` → `"HELLO"` |

### Trimming

| Function | Description | Example |
|----------|-------------|---------|
| `trim(s)` | Trim whitespace | `trim("  hi  ")` → `"hi"` |
| `ltrim(s)` | Trim left whitespace | `ltrim("  hi")` → `"hi"` |
| `rtrim(s)` | Trim right whitespace | `rtrim("hi  ")` → `"hi"` |

### Searching

| Function | Description | Example |
|----------|-------------|---------|
| `contains(s, sub)` | Check if contains substring | `contains("hello", "ell")` → `1` |
| `starts_with(s, prefix)` | Check if starts with | `starts_with("hello", "he")` → `1` |
| `ends_with(s, suffix)` | Check if ends with | `ends_with("hello", "lo")` → `1` |
| `index_of(s, sub)` | Find first occurrence | `index_of("hello", "l")` → `2` |
| `last_index_of(s, sub)` | Find last occurrence | `last_index_of("hello", "l")` → `3` |
| `find(s, sub [, start])` | Python-style forward search | `find("ababa", "ba", 2)` → `3` |
| `rfind(s, sub [, end])` | Reverse search in an optional prefix | `rfind("ababa", "ba")` → `3` |
| `find_all(s, sub)` | Non-overlapping byte indexes of a substring | `find_all("aaaa", "aa")` → `[0,2]` |
| `find_all_overlapping(s, sub)` | Overlapping byte indexes of a substring | `find_all_overlapping("aaaa", "aa")` → `[0,1,2]` |

### Manipulation

| Function | Description | Example |
|----------|-------------|---------|
| `substr(s, start, len)` | Extract substring | `substr("hello", 1, 3)` → `"ell"` |
| `slice(s, start [, end])` | Byte slice with negative indices | `slice("abcdef", -4, -1)` → `"cde"` |
| `char_at(s, index)` | Byte at index as a string | `char_at("abc", 1)` → `"b"` |
| `byte_at(s, index)` | Byte value at index | `byte_at("Az", 1)` → `122` |
| `byte_length(s)` | Byte length alias | `byte_length("你")` → `3` |
| `utf8_slice(s, start [, end])` | UTF-8 codepoint slice with negative indices | `utf8_slice("a你b", -2)` → `"你b"` |
| `ord(s [, index])` | UTF-8 codepoint value | `ord("你")` → `20320` |
| `chr(codepoint)` | UTF-8 string from one codepoint | `chr(20320)` → `"你"` |
| `from_codepoint(cp...)` | UTF-8 string from codepoints | `from_codepoint(20320,128578)` → `"你🙂"` |
| `codepoint_at(s, index)` | Alias for indexed `ord` | `codepoint_at("a你", 1)` → `20320` |
| `replace(s, from, to)` | Replace all occurrences | `replace("aabbcc", "bb", "XX")` → `"aaXXcc"` |
| `replace_all(s, from, to)` | Alias for `replace` | `replace_all("aabb", "a", "x")` → `"xxbb"` |
| `replace_range(s, start, len, value)` | Replace a byte range | `replace_range("abcdef", 2, 3, "X")` → `"abXf"` |
| `insert(s, index, value)` | Insert at a byte index | `insert("ab", 1, "X")` → `"aXb"` |
| `delete_range(s, start, len)` | Delete a byte range | `delete_range("abcdef", 2, 3)` → `"abf"` |
| `reverse(s)` | Reverse string | `reverse("abc")` → `"cba"` |
| `repeat(s, count)` | Alias for `str_repeat` | `repeat("ab", 2)` → `"abab"` |
| `count(s, sub)` | Alias for substring count | `count("aaaa", "aa")` → `2` |
| `count_overlapping(s, sub)` | Count overlapping substring occurrences | `count_overlapping("aaaa", "aa")` → `3` |
| `left(s, count)` / `str_take(s, count)` | Take leading UTF-8 codepoints when valid | `left("a你b", 2)` → `"a你"` |
| `right(s, count)` | Take trailing UTF-8 codepoints when valid | `right("a你b", 2)` → `"你b"` |
| `str_drop(s, count)` | Drop leading UTF-8 codepoints when valid | `str_drop("a你b", 1)` → `"你b"` |
| `normalize_space(s)` | Trim and collapse ASCII whitespace | `normalize_space(" a\t b ")` → `"a b"` |
| `center(s, width [, fill])` | Center by UTF-8 codepoints when valid | `center("x", 3, ".")` → `".x."` |
| `truncate(s, max_len [, suffix])` | Truncate by UTF-8 codepoints when valid | `truncate("abcdef", 5)` → `"ab..."` |
| `zfill(s, width)` | Left-pad with zeros after an optional sign | `zfill("-42", 5)` → `"-0042"` |
| `expand_tabs(s [, tabsize])` | Expand tabs to spaces | `expand_tabs("a\tb", 4)` → `"a   b"` |
| `chomp(s)` | Remove one trailing line ending | `chomp("a\r\n")` → `"a"` |
| `split_lines(s)` | Alias for `str_lines` | `split_lines("a\nb")` → `["a","b"]` |
| `line_count(s)` | Count logical lines like `str_lines` | `line_count("a\nb")` → `2` |
| `indent(s, prefix [, first])` | Prefix lines; `first=0` skips the first line | `indent("a\nb", "> ")` → `"> a\n> b"` |
| `dedent(s)` / `unindent(s)` | Remove common leading spaces/tabs from non-blank lines | `dedent("  a\n    b")` → `"a\n  b"` |
| `surround(s, prefix [, suffix])` | Add paired prefix/suffix | `surround("x", "[", "]")` → `"[x]"` |
| `unwrap(s, prefix [, suffix])` | Remove paired prefix/suffix if both match | `unwrap("[x]", "[", "]")` → `"x"` |
| `word_wrap(s, width [, break_long])` | Wrap ASCII words to a byte width | `word_wrap("alpha beta", 5)` → `"alpha\nbeta"` |
| `shorten(s, width [, suffix])` | Shorten at a word boundary | `shorten("alpha beta", 8)` → `"alpha..."` |
| `snake_case(s)` / `kebab_case(s)` | Convert ASCII words to delimited field names | `snake_case("Hello World")` → `"hello_world"` |
| `camel_case(s)` / `pascal_case(s)` | Convert ASCII words to camel/Pascal case | `camel_case("hello world")` → `"helloWorld"` |
| `slugify(s)` | Convert ASCII words to a URL slug | `slugify("Hello, Turbo!")` → `"hello-turbo"` |
| `strip(s)` / `lstrip(s)` / `rstrip(s)` | Aliases for trim variants | `strip(" hi ")` → `"hi"` |
| `strip_prefix(s, prefix)` | Alias for `remove_prefix` | `strip_prefix("pre-x", "pre-")` → `"x"` |
| `strip_suffix(s, suffix)` | Alias for `remove_suffix` | `strip_suffix("x.txt", ".txt")` → `"x"` |
| `padStart(s, width [, fill])` / `padEnd(...)` | JS-style padding aliases | `padStart("x", 3, "0")` → `"00x"` |
| `split_whitespace(s)` | Split on ASCII whitespace into strings | `split_whitespace(" a b ")` → `["a","b"]` |
| `split_once(s, delim)` | Split at first delimiter into `[before, after]` | `split_once("a=b=c", "=")` → `["a","b=c"]` |
| `rsplit_once(s, delim)` | Split at last delimiter into `[before, after]` | `rsplit_once("a=b=c", "=")` → `["a=b","c"]` |
| `split_limit(s, delim, maxsplit)` | Split from the left with a split limit | `split_limit("a,b,c", ",", 1)` → `["a","b,c"]` |
| `rsplit(s, delim [, maxsplit])` | Split from the right into strings | `rsplit("a,b,c", ",", 1)` → `["a,b","c"]` |

### Predicates

| Function | Description | Example |
|----------|-------------|---------|
| `is_empty(s)` | String has zero bytes | `is_empty("")` → `1` |
| `is_blank(s)` | Empty or ASCII whitespace only | `is_blank(" \t")` → `1` |
| `is_ascii(s)` | All bytes are ASCII | `is_ascii("abc")` → `1` |
| `is_digit(s)` | Non-empty ASCII digits only | `is_digit("123")` → `1` |
| `is_hex(s)` | Non-empty ASCII hexadecimal digits only | `is_hex("0aF9")` → `1` |
| `is_alpha(s)` | Non-empty ASCII letters only | `is_alpha("abc")` → `1` |
| `is_alnum(s)` | Non-empty ASCII letters/digits only | `is_alnum("abc123")` → `1` |
| `is_space(s)` | Non-empty ASCII whitespace only | `is_space("\n")` → `1` |
| `is_printable(s)` | ASCII printable bytes plus common whitespace | `is_printable("a\n")` → `1` |
| `is_lower(s)` | Has lowercase letters and no uppercase letters | `is_lower("abc1")` → `1` |
| `is_upper(s)` | Has uppercase letters and no lowercase letters | `is_upper("ABC1")` → `1` |
| `constant_time_eq(a, b)` | Byte equality without early exit | `constant_time_eq("x","x")` → `1` |

### Encoding and Escaping

| Function | Description | Example |
|----------|-------------|---------|
| `html_escape(s)` | Escape HTML-sensitive characters | `html_escape("<a&>")` → `"&lt;a&amp;&gt;"` |
| `html_unescape(s)` | Decode common HTML entities | `html_unescape("&lt;")` → `"<"` |
| `json_escape(s)` | Escape a string for JSON string literal content | `json_escape("a\n")` → `"a\\n"` |
| `json_unescape(s)` | Decode JSON string literal escapes | `json_unescape("\\u4F60")` → `"你"` |
| `url_encode(s)` | Percent-encode UTF-8 bytes | `url_encode("a b")` → `"a%20b"` |
| `url_decode(s)` | Decode percent escapes (`+` becomes space) | `url_decode("a%20b")` → `"a b"` |
| `base64_encode(s)` | Base64 encode bytes | `base64_encode("hello")` → `"aGVsbG8="` |
| `base64_decode(s)` | Base64 decode to a string | `base64_decode("aGVsbG8=")` → `"hello"` |
| `base64url_encode(s)` | URL-safe Base64 without padding | `base64url_encode("hello?")` → `"aGVsbG8_"` |
| `base64url_decode(s)` | Decode URL-safe Base64 | `base64url_decode("aGVsbG8_")` → `"hello?"` |
| `hex_encode(s)` | Lowercase hex encode bytes | `hex_encode("Az")` → `"417a"` |
| `hex_decode(s)` | Hex decode to a string | `hex_decode("417a")` → `"Az"` |
| `bytes(s)` | Return a native byte buffer | `bytes("Az")` |
| `from_bytes(bytes/list/vector)` | Build a string from byte values | `from_bytes(bytes("Az"))` → `"Az"` |

### Hash Plugin

Load with `import("hash")`.

| Function | Description | Example |
|----------|-------------|---------|
| `hash.xxh32(s)` | xxHash32 as an exact numeric value | `hash.xxh32("abc")` |
| `hash.xxh32(s, seed)` | Seeded xxHash32 | `hash.xxh32("abc", 7)` |
| `hash.xxh64_hex(s)` | xxHash64 as 16 lowercase hex bytes | `hash.xxh64_hex("abc")` |
| `hash.xxh64_hex(s, seed)` | Seeded xxHash64 hex | `hash.xxh64_hex("abc", 7)` |
| `hash.xxh3_64_hex(s)` | XXH3 64-bit hash as 16 lowercase hex bytes | `hash.xxh3_64_hex("abc")` |
| `hash.xxh3_64_hex(s, seed)` | Seeded XXH3 64-bit hash hex | `hash.xxh3_64_hex("abc", 7)` |

### Fuzzy Plugin

Load with `import("fuzzy")`. Search functions return a map with
`matched`, `start`, `end`, `utf8_start`, `utf8_end`, and `text`; fuzzy
functions also return `cost`, `insertions`, `deletions`, and `substitutions`.
Offsets `start`/`end` are byte offsets; `utf8_start`/`utf8_end` are UTF-8
codepoint offsets.

Automaton helpers return a list of match maps. `fuzzy.levenshtein` and
`fuzzy.agrep` use TurboUtils Levenshtein automata. `fuzzy.ac` uses
TurboUtils Aho-Corasick multi-pattern matching. Their optional `mode` is
`"utf8"` by default or `"byte"` for raw byte matching.

Flags are a string: `i` case-insensitive, `n` newline mode, `l` literal mode,
and `r` regex mode. `fuzzy.search` is literal by default; use
`fuzzy.fuzzy_regex_search` for approximate regex matching.

| Function | Description | Example |
|----------|-------------|---------|
| `fuzzy.search(pattern, text [, max_cost [, flags]])` | UTF-8 fuzzy literal search | `fuzzy.search("cafe", "xx cafe yy", 1)` |
| `fuzzy.fuzzy_search(pattern, text [, max_cost [, flags]])` | Alias for `fuzzy.search` | `fuzzy.fuzzy_search("cafe", "xx cafe yy", 1)` |
| `fuzzy.regex(pattern, text [, flags])` | UTF-8 exact regex search | `fuzzy.regex("caf.", "xx cafe yy")` |
| `fuzzy.regex_search(pattern, text [, flags])` | Alias for `fuzzy.regex` | `fuzzy.regex_search("caf.", "xx cafe yy")` |
| `fuzzy.fuzzy_regex_search(pattern, text [, max_cost [, flags]])` | UTF-8 fuzzy regex search | `fuzzy.fuzzy_regex_search("br.ve", "brave", 0)` |
| `fuzzy.levenshtein(pattern, text, max_distance [, mode])` | List all Levenshtein automaton matches | `fuzzy.levenshtein("cafe", text, 1)` |
| `fuzzy.lev(...)` / `fuzzy.agrep(...)` | Aliases for `fuzzy.levenshtein` | `fuzzy.agrep("optimise", text, 2)` |
| `fuzzy.ac(patterns, text [, mode])` | Aho-Corasick multi-pattern search; `patterns` is a string or list of strings | `fuzzy.ac(["err","warn"], log)` |
| `fuzzy.multi_search(...)` | Alias for `fuzzy.ac` | `fuzzy.multi_search(["foo","bar"], text)` |
| `fuzzy.tre_version()` | TRE runtime version | `fuzzy.tre_version()` |

### Crypto Plugin

Load with `import("crypto")`. Hash outputs are lowercase hexadecimal strings.
Binary inputs may be `bytes` or strings; encryption APIs return `bytes`.

| Function | Description | Example |
|----------|-------------|---------|
| `crypto.sha256(s)` | SHA-256 as a 32-byte digest encoded in lowercase hex | `crypto.sha256("abc")` |
| `crypto.sha256_bytes(s)` | SHA-256 as raw 32-byte digest bytes | `crypto.sha256_bytes("abc")` |
| `crypto.blake2b(s)` | BLAKE2b, 64-byte digest | `crypto.blake2b("abc")` |
| `crypto.blake2b(s, size)` | BLAKE2b with digest size `1..64` bytes | `crypto.blake2b("abc", 4)` → `"63906248"` |
| `crypto.blake2b_keyed(s, key)` | Keyed BLAKE2b, 64-byte digest | `crypto.blake2b_keyed("abc", "key")` |
| `crypto.blake2b_keyed(s, key, size)` | Keyed BLAKE2b with digest size `1..64` bytes | `crypto.blake2b_keyed("abc", "key", 4)` → `"34d401b4"` |
| `crypto.aes_encrypt(data, key, iv)` | AES-CTR encryption with a 16-byte key and 16-byte IV | `crypto.aes_encrypt(raw, key, iv)` |
| `crypto.aes_decrypt(data, key, iv)` | AES-CTR decryption with the same parameters | `crypto.aes_decrypt(cipher, key, iv)` |
| `crypto.aead_lock(data, key, nonce, ad)` | Monocypher XChaCha20-Poly1305 AEAD with 32-byte key and 24-byte nonce; returns `{cipher, mac}` | `crypto.aead_lock(raw, key, nonce, ad)` |
| `crypto.aead_unlock(cipher, mac, key, nonce, ad)` | Verifies and decrypts AEAD data; returns `null` on authentication failure | `crypto.aead_unlock(cipher, mac, key, nonce, ad)` |
| `crypto.argon2(pass, salt, out_len, blocks, passes, algo)` | Argon2 KDF; `algo`: `0` Argon2d, `1` Argon2i, `2` Argon2id | `crypto.argon2(pw, salt, 32, 65536, 3, 2)` |
| `crypto.argon2(pass, salt, out_len, blocks, passes, algo, key, ad)` | Argon2 KDF with optional secret key and associated data | `crypto.argon2(pw, salt, 32, 65536, 3, 2, k, ad)` |
| `crypto.x25519_public_key(secret32)` | X25519 public key from a 32-byte secret | `crypto.x25519_public_key(sk)` |
| `crypto.x25519(secret32, public32)` | X25519 raw shared secret; hash it before use as a key | `crypto.x25519(sk, peer_pk)` |
| `crypto.eddsa_key_pair(seed32)` | Monocypher EdDSA key pair; returns `{secret_key, public_key}` | `crypto.eddsa_key_pair(seed)` |
| `crypto.eddsa_sign(secret64, message)` | Sign a message, returning a 64-byte signature | `crypto.eddsa_sign(sk, msg)` |
| `crypto.eddsa_check(sig64, public32, message)` | Verify an EdDSA signature, returning `1` or `0` | `crypto.eddsa_check(sig, pk, msg)` |
| `crypto.chacha20_x(data, key32, nonce24[, ctr])` | XChaCha20 stream cipher; unauthenticated, prefer AEAD for messages | `crypto.chacha20_x(raw, key, nonce, 0)` |
| `crypto.chacha20_djb(data, key32, nonce8[, ctr])` | ChaCha20 DJB stream cipher | `crypto.chacha20_djb(raw, key, nonce)` |
| `crypto.chacha20_ietf(data, key32, nonce12[, ctr])` | ChaCha20 IETF stream cipher | `crypto.chacha20_ietf(raw, key, nonce)` |
| `crypto.chacha20_h(key32, in16)` | HChaCha20 32-byte subkey derivation | `crypto.chacha20_h(key, input)` |
| `crypto.poly1305(message, key32)` | Poly1305 one-time authenticator, returning 16 bytes | `crypto.poly1305(msg, one_time_key)` |
| `crypto.elligator_key_pair(seed32)` | Elligator-compatible key pair, returning `{hidden, secret_key}` | `crypto.elligator_key_pair(seed)` |
| `crypto.elligator_map(hidden32)` | Map an Elligator representative to a Curve25519 point | `crypto.elligator_map(hidden)` |
| `crypto.elligator_rev(curve32, tweak)` | Reverse Elligator map; returns `null` if not representable | `crypto.elligator_rev(point, 0)` |
| `crypto.verify16(a, b)` | Constant-time compare of two 16-byte strings | `crypto.verify16(mac1, mac2)` |
| `crypto.verify32(a, b)` | Constant-time compare of two 32-byte strings | `crypto.verify32(d1, d2)` |
| `crypto.verify64(a, b)` | Constant-time compare of two 64-byte strings | `crypto.verify64(d1, d2)` |

Additional low-level helpers are exposed for advanced protocols:
`x25519_to_eddsa`, `x25519_inverse`, `x25519_dirty_small`,
`x25519_dirty_fast`, `eddsa_to_x25519`, `eddsa_trim_scalar`,
`eddsa_reduce`, `eddsa_mul_add`, and `eddsa_scalarbase`.

### Parsing

| Function | Description | Example |
|----------|-------------|---------|
| `split(s, delim)` | Split into vector | `split("1,2,3", ",")` → `[1,2,3]` |
| `tokenize(s, delim, idx)` | Get n-th token | `tokenize("a,b,c", ",", 1)` → `"b"` |
| `token_count(s, delim)` | Count tokens | `token_count("a,b,c", ",")` → `3` |

### Template Rendering

| Function | Description | Example |
|----------|-------------|---------|
| `template_render(template, data)` | Render a Mustache template from native script data | `template_render("Hi {{name}}", map{name:"Ada"})` → `"Hi Ada"` |
| `template_render(template, data, partials)` | Render with partial templates from a map | `template_render("{{>item}}", data, map{item:"{{name}}"})` |
| `mustache_render(template, data)` | Alias for `template_render` | `mustache_render("{{name}}", data)` |

`data` may be a map, list, vector, scalar, or function value. Maps support field
lookup and dotted paths such as `{{user.city}}`. Lists and vectors support
sections such as `{{#items}}{{name}}{{/items}}`; scalar truthy values render a
section once. `{{name}}` HTML-escapes output, while `{{{name}}}` writes raw text.
Missing fields and missing partials render as empty output. Partial names also
support dotted paths into the `partials` map, such as `{{>layout.item}}`.

Function values act as Mustache lambdas. Interpolation lambdas receive an empty
string; section lambdas receive the raw section body. The lambda result is
converted to text and rendered through the current template context.

```javascript
var data = map{
    name: "Ada",
    user: map{city: "Paris"},
    items: list(map{name: "one"}, map{name: "two"}),
    wrap: func(raw) { return "[" + raw + ":" + "{{name}}" + "]"; }
};

var partials = map{item: "{{name}};"};
var out = template_render(
    "Hi {{name}} {{user.city}} {{#items}}{{>item}}{{/items}} {{#wrap}}body{{/wrap}}",
    data,
    partials
);
// "Hi Ada Paris one;two; [body:Ada]"
```

### Dot-Style Methods

All string functions support dot-style syntax:

```javascript
"HELLO".lower()              // "hello"
"hello".upper()              // "HELLO"
"  hi  ".trim()              // "hi"
"hello".contains("ell")      // 1
"hello".indexOf("l")         // 2
"hello".substr(1, 3)         // "ell"
"aabbcc".replace("bb", "XX") // "aaXXcc"
"hello".length()             // 5
"abc".reverse()              // "cba"
```

---

## Vector Functions

### Basic Operations

| Function | Description | Example |
|----------|-------------|---------|
| `size(v)` | Number of elements | `size([1,2,3])` → `3` |
| `len(v)` | Alias for size | `len([1,2,3])` → `3` |

### Search

| Function | Description | Example |
|----------|-------------|---------|
| `vector_find_value(v, val)` | First index of value | `vector_find_value([10,20,30], 20)` → `1` |
| `vector_find_all(v, val)` | All indices of value | `vector_find_all([10,20,10], 10)` → `[0,2]` |

### Dot-Style Methods

```javascript
var v = [10, 20, 30, 40, 50];

// Built-in methods
v.length()              // 5
v.push(60)              // Append element (mutates)
v.pop()                 // Remove last (mutates)
v.indexOf(30)           // 2
v.reverse()             // Reversed copy

// Registry-dispatched methods
v.sum()                 // 150
v.avg()                 // 30
v.min()                 // 10
v.max()                 // 50
v.sort()                // Sorted copy
v.median()              // 30
v.cumsum()              // [10, 30, 60, 100, 150]
```

---

## Linear Algebra

Matrices are stored **column-major** in flat vectors, matching the `miniblas/linalg`
backend used by core matrix operations.
Functions backed by `miniblas/linalg` also accept an explicit layout flag as
the final argument. Omit it or pass `0` for column-major storage; pass `1` for
Eigen-style row-major storage.

### 2×2 Matrices

| Function | Description | Example |
|----------|-------------|---------|
| `det2(m)` | Determinant | `det2([1,2,3,4])` → `-2` |
| `inv2(m)` | Inverse matrix | `inv2([1,2,3,4])` → `[-2,1,1.5,-0.5]` |
| `trace2(m)` | Trace (sum of diagonal) | `trace2([1,2,3,4])` → `5` |
| `eig2(m)` | Eigenvalues | `eig2([1,2,3,4])` → `[λ1, λ2]` |

### 3×3 Matrices

| Function | Description | Example |
|----------|-------------|---------|
| `det3(m)` | Determinant | `det3([1,0,0,0,1,0,0,0,1])` → `1` |
| `inv3(m)` | Inverse matrix | `inv3([...])` → `[...]` |
| `eig3(m, out)` | Eigenvalues (stored in out) | `eig3(m, result)` |

### General Operations

| Function | Description | Example |
|----------|-------------|---------|
| `matrix.add(A, B, rows, cols[, layout])` | Elementwise matrix addition | `matrix.add(A, B, 2, 3)` |
| `matrix.sub(A, B, rows, cols[, layout])` | Elementwise matrix subtraction | `matrix.sub(A, B, 2, 3)` |
| `matrix.hadamard(A, B, rows, cols[, layout])` / `matrix.mul(...)` | Elementwise matrix product | `matrix.hadamard(A, B, 2, 3)` |
| `matrix.scale(A, rows, cols, scalar[, layout])` | Scale every matrix element | `matrix.scale(A, 2, 3, 0.5)` |
| `matmul(A, B, m, k, n[, layout])` | Matrix multiply (m×k) × (k×n) | `matmul(A, B, 2, 2, 2, 1)` |
| `matrix.copy(A, rows, cols[, layout])` | Copy the matrix flat vector | `matrix.copy(A, 2, 3)` |
| `matrix.flatten(A, rows, cols[, layout])` | Return a flat matrix copy | `matrix.flatten(A, 2, 3)` |
| `matrix.reshape(A, old_rows, old_cols, new_rows, new_cols[, layout])` | Validate shape-compatible reshape and return a flat copy | `matrix.reshape(A, 2, 3, 3, 2)` |
| `matrix.slice(A, rows, cols, row0, row_count, col0, col_count[, layout])` | Extract a matrix block | `matrix.slice(A, 4, 4, 1, 2, 1, 2)` |
| `matrix.identity(n[, layout])` | Create an n×n identity matrix | `matrix.identity(3)` |
| `matrix.eye_like(A, rows, cols[, layout])` | Create an identity-shaped matrix with the same dimensions | `matrix.eye_like(A, 2, 3)` |
| `matrix.dot(a, b)` | Vector dot product | `matrix.dot([1,2], [3,4])` → `11` |
| `matrix.outer(a, b[, layout])` | Vector outer product | `matrix.outer([1,2], [10,20])` |
| `matrix.add_row(A, row, rows, cols[, layout])` | Broadcast-add one row vector across all rows | `matrix.add_row(A, [1,2,3], 2, 3)` |
| `matrix.sub_row(A, row, rows, cols[, layout])` | Broadcast-subtract one row vector | `matrix.sub_row(A, row, 2, 3)` |
| `matrix.mul_row(A, row, rows, cols[, layout])` | Broadcast-multiply one row vector | `matrix.mul_row(A, row, 2, 3)` |
| `matrix.div_row(A, row, rows, cols[, layout])` | Broadcast-divide by one row vector | `matrix.div_row(A, row, 2, 3)` |
| `matrix.add_col(A, col, rows, cols[, layout])` | Broadcast-add one column vector across all columns | `matrix.add_col(A, [1,2], 2, 3)` |
| `matrix.sub_col(A, col, rows, cols[, layout])` | Broadcast-subtract one column vector | `matrix.sub_col(A, col, 2, 3)` |
| `matrix.mul_col(A, col, rows, cols[, layout])` | Broadcast-multiply one column vector | `matrix.mul_col(A, col, 2, 3)` |
| `matrix.div_col(A, col, rows, cols[, layout])` | Broadcast-divide by one column vector | `matrix.div_col(A, col, 2, 3)` |
| `matrix.sum(A, rows, cols[, axis][, layout])` | Sum all elements, or reduce by `"col"`/`0` or `"row"`/`1` | `matrix.sum(A, 2, 3, "row", 1)` |
| `matrix.mean(A, rows, cols[, axis][, layout])` | Mean of all elements, or reduce by axis | `matrix.mean(A, 2, 3, 0)` |
| `matrix.min(A, rows, cols[, axis][, layout])` | Minimum of all elements, or reduce by axis | `matrix.min(A, 2, 3, "col")` |
| `matrix.max(A, rows, cols[, axis][, layout])` | Maximum of all elements, or reduce by axis | `matrix.max(A, 2, 3, "row")` |
| `matrix.variance(A, rows, cols[, axis][, layout])` / `matrix.var(...)` | Population variance globally or by axis | `matrix.variance(A, 2, 3)` |
| `matrix.std(A, rows, cols[, axis][, layout])` | Population standard deviation globally or by axis | `matrix.std(A, 2, 3, "col")` |
| `matrix.argmin(A, rows, cols[, axis][, layout])` | Index of minimum globally or by axis | `matrix.argmin(A, 2, 3)` |
| `matrix.argmax(A, rows, cols[, axis][, layout])` | Index of maximum globally or by axis | `matrix.argmax(A, 2, 3)` |
| `matrix.row(A, rows, cols, index[, layout])` | Extract a row as a vector | `matrix.row(A, 2, 3, 1)` |
| `matrix.col(A, rows, cols, index[, layout])` | Extract a column as a vector | `matrix.col(A, 2, 3, 0, 1)` |
| `matrix.det(A, n[, layout])` | Generic n×n determinant using Gaussian elimination with partial pivoting | `matrix.det(A, 3)` |
| `matrix.diag(A, rows, cols[, layout])` | Extract the main diagonal as a vector | `matrix.diag(A, 3, 2)` |
| `matrix.zeros(rows, cols[, layout])` | Create a dense zero matrix | `matrix.zeros(2, 3)` |
| `matrix.ones(rows, cols[, layout])` | Create a dense matrix filled with `1` | `matrix.ones(2, 3, 1)` |
| `matrix.full(rows, cols, value[, layout])` | Create a dense matrix filled with `value` | `matrix.full(2, 3, 7)` |
| `matrix.inv(A, n[, layout])` | Generic n×n inverse using the double Gauss-Jordan backend | `matrix.inv(A, 3)` |
| `matrix.norm(A, rows, cols[, kind][, layout])` | Matrix norm: default/Frobenius, `"l1"` max column sum, `"inf"` max row sum | `matrix.norm(A, 3, 2, "l1")` |
| `matrix.rank(A, rows, cols[, tol][, layout])` | Numerical rank by Gaussian elimination | `matrix.rank(A, 3, 2)` |
| `matrix.cond(A, rows, cols[, layout])` | 2-norm condition estimate from singular values | `matrix.cond(A, 3, 2)` |
| `matrix.slogdet(A, n[, layout])` | Return `[sign, log(abs(det))]` using LU determinant | `matrix.slogdet(A, 3)` |
| `matrix.solve(A, B, n, cols[, layout])` | Solve `A * X = B`, where `B` has `cols` columns | `matrix.solve(A, b, 3, 1)` |
| `matrix.shape(A, rows, cols[, layout])` | Validate a flat matrix and return `[rows, cols]` | `matrix.shape(A, 3, 2)` |
| `matrix.info(A, rows, cols[, layout])` | Return metadata map: `rows`, `cols`, `size`, `layout`, `order`, `dtype`, `backend_dtype` | `matrix.info(A, 3, 2)` |
| `matrix.rows(A, rows, cols[, layout])` | Validate and return row count | `matrix.rows(A, 3, 2)` |
| `matrix.cols(A, rows, cols[, layout])` | Validate and return column count | `matrix.cols(A, 3, 2)` |
| `matrix.trace(A, rows, cols[, layout])` | Sum the main diagonal | `matrix.trace(A, 3, 2)` |
| `matrix.transpose(A, rows, cols[, layout])` | Transpose while preserving the declared layout | `matrix.transpose(A, 2, 3, 1)` |
| `transpose(m, rows, cols)` | Transpose matrix | `transpose([1,2,3,4], 2, 2)` → `[1,3,2,4]` |

Most `matrix.*` helpers also have global `mat_*` aliases, including
`mat_copy`, `mat_flatten`, `mat_reshape`, `mat_slice`, `mat_dot`, `mat_outer`,
`mat_identity`, `mat_eye_like`, `mat_var`, `mat_std`, `mat_argmin`, `mat_argmax`,
and the existing arithmetic/reduction aliases.
For `matrix.norm(A, rows, cols, x)`, a numeric `x` is interpreted as the layout flag;
use string kinds such as `"l1"` or `"inf"` when passing four arguments.
For matrix reductions, the fourth argument is always the axis; pass layout as the fifth
argument: `matrix.sum(A, rows, cols, 0, 1)`.
In source text, prefer `matrix.variance(...)` or `mat_var(...)` over
`matrix.var(...)`, because `var` is also a declaration keyword in the grammar.

### Linalg Helpers

| Function | Description | Example |
|----------|-------------|---------|
| `linalg.qr(A, rows, cols[, layout])` | Return `[Q, R]` using Gram-Schmidt QR | `linalg.qr(A, 3, 2)` |
| `linalg.lu(A, n[, layout])` | Return `[L, U, pivots]` from partial-pivot LU | `linalg.lu(A, 3)` |
| `linalg.lu_solve(A, B, n, cols[, layout])` | Solve `A * X = B` through LU decomposition | `linalg.lu_solve(A, b, 3, 1)` |
| `linalg.solve_cholesky(A, B, n, cols[, layout])` | Solve SPD systems through Cholesky and triangular solves | `linalg.solve_cholesky(A, b, 3, 1)` |
| `linalg.det_lu(A, n[, layout])` | Determinant through LU diagonal product | `linalg.det_lu(A, 3)` |
| `linalg.rank(A, rows, cols[, tol][, layout])` | Alias for `matrix.rank` | `linalg.rank(A, 3, 2)` |
| `linalg.cond(A, rows, cols[, layout])` | Alias for `matrix.cond` | `linalg.cond(A, 3, 2)` |
| `linalg.eigh(A, n[, layout])` | Eigenvalues of a real symmetric matrix, ascending | `linalg.eigh(A, 3)` |
| `linalg.pinv(A, rows, cols[, layout])` | Pseudoinverse for square or tall full-rank matrices | `linalg.pinv(A, 4, 2)` |
| `linalg.lstsq(A, b, rows, cols[, layout])` | Least-squares solution using pseudoinverse | `linalg.lstsq(A, b, 4, 2)` |
| `linalg.svd(A, rows, cols[, layout])` / `linalg.svd2(...)` | Singular values sorted descending | `linalg.svd([3,0,0,4], 2, 2)` |
| `linalg.eig2(A)` | 2×2 eigenvalues alias | `linalg.eig2([1,2,3,4])` |

**Example:**
```javascript
// 2×2 column-major matrix: [[1, 3], [2, 4]]
var A = [1, 2, 3, 4];

// Determinant
var d = det2(A);  // -2

// Inverse
var inv = inv2(A);  // [-2, 1, 1.5, -0.5]

// Matrix multiplication: A × A, result is also column-major
var result = matmul(A, A, 2, 2, 2);  // [7, 10, 15, 22]

// Explicit row-major input/output
var row = matmul([1, 2, 3, 4], [5, 6, 7, 8], 2, 2, 2, 1);
// row == [19, 22, 43, 50]

var ewise = matrix.hadamard([1, 2, 3, 4], [10, 20, 30, 40], 2, 2);
// ewise == [10, 40, 90, 160]

var colSums = matrix.sum([1, 4, 2, 5, 3, 6], 2, 3, "col");
// colSums == [5, 7, 9]

var row1 = matrix.row([1, 2, 3, 4, 5, 6], 2, 3, 1, 1);
// row1 == [4, 5, 6]

var meta = matrix.info(A, 2, 2);
// meta.order == "col", meta.dtype == "f64", meta.backend_dtype == "f32"

var filled = matrix.full(2, 3, 7);
// filled == [7, 7, 7, 7, 7, 7]

var det = matrix.det([1, 0, 5, 2, 1, 6, 3, 4, 0], 3);
// det == 1

var l1 = matrix.norm([1, -4, -2, 5, 3, -6], 2, 3, "l1");
// l1 == 9

var diag = matrix.diag([1, 3, 5, 2, 4, 6], 3, 2);
// diag == [1, 4], matrix.trace(...) == 5

var t = matrix.transpose([1, 2, 3, 4, 5, 6], 2, 3, 1);
// t == [1, 4, 2, 5, 3, 6]

var x = matrix.solve([2, 1, 1, 3], [5, 10], 2, 1);
// x == [1, 3]

var block = matrix.slice([1, 2, 3, 4, 5, 6], 2, 3, 0, 2, 1, 2);
var centered = matrix.sub_col([1, 4, 2, 5, 3, 6], [2.5, 3.5], 2, 3);
var s = linalg.svd([3, 0, 0, 4], 2, 2);
var tt = stats.t_test_1samp([1, 2, 3, 4], 2.5);
```

---

## Tabular Data Helpers

`table.*` helpers operate on `list(map{...})` values and return native lists or maps.

| Function | Description | Example |
|----------|-------------|---------|
| `table.select(rows, field...)` | Project selected fields from each row | `table.select(rows, "id", "value")` |
| `table.filter(rows, predicate)` | Keep rows where a script function or function name returns truthy | `table.filter(rows, "keep_big")` |
| `table.groupby(rows, key, value[, op])` | Group by `key` and aggregate `value`; `op` is `"sum"`, `"mean"`, or `"count"` | `table.groupby(rows, "group", "value", "mean")` |
| `table.join(left, right, key)` | Inner join two row lists by a key field | `table.join(a, b, "id")` |

```javascript
var rows = list(map{id:1, group:"a", value:10}, map{id:2, group:"a", value:20});
var selected = table.select(rows, "id", "value");
var grouped = table.groupby(rows, "group", "value", "mean");
// grouped.a.value == 15
```

---

## File I/O

### File Operations

| Function | Description | Returns |
|----------|-------------|---------|
| `read_file(path)` | Read file contents | String (or 0 on error) |
| `write_file(path, content)` | Write to file | 0 on success |
| `append_file(path, content)` | Append to file | 0 on success |
| `copy_file(src, dst)` | Copy file contents | 0 on success |
| `file_exists(path)` | Check if file exists | 1 if exists, 0 otherwise |
| `file_size(path)` | Get file size | Bytes (-1 on error) |
| `file_stat(path)` | Get file info | `[size, mtime, is_file, is_dir]` |
| `file_truncate(path, length)` | Truncate or extend a file | 0 on success |
| `is_file(path)` | Check if regular file | 1 if file, 0 otherwise |
| `is_dir(path)` | Check if directory | 1 if directory, 0 otherwise |
| `file_remove(path)` | Delete file | 0 on success |
| `file_rename(old, new)` | Rename/move file | 0 on success |

**Example:**
```javascript
// Write file
write_file("output.txt", "Hello, World!");

// Read file
var content = read_file("output.txt");
print(content);  // "Hello, World!"

// Check existence
if (file_exists("data.txt")) {
    var size = file_size("data.txt");
    print("File size: " + size + " bytes");
}

// Delete file
file_remove("temp.txt");
```

### Directory Operations

| Function | Description | Returns |
|----------|-------------|---------|
| `listdir(path)` | List directory entry names | List |
| `glob(pattern)` | List paths matching a wildcard pattern | List |
| `mkdir(path [, mode])` | Create directory | 0 on success |
| `mkdir_recursive(path [, mode])` | Create a directory and missing parents | 0 on success |
| `rmdir(path)` | Remove directory | 0 on success |
| `rmdir_recursive(path)` | Remove a directory tree | 0 on success |
| `tmpdir()` | Get temp directory path | String |

File, path, and directory primitives are backed by TurboNet `turbo_fs`.
`listdir()` and `glob()` return script lists; mutation APIs keep the IO
status convention of `0` on success and a negative value on failure.

### Path Utilities

| Function | Description | Returns |
|----------|-------------|---------|
| `path_join(base, rel)` | Join path components | String |
| `path_dirname(path)` | Get directory part | String |
| `path_basename(path)` | Get filename part | String |
| `path_is_absolute(path)` | Check if absolute path | 1 if absolute, 0 otherwise |

**Example:**
```javascript
var full = path_join("/home/user", "data.csv");  // "/home/user/data.csv"
var dir = path_dirname(full);                     // "/home/user"
var file = path_basename(full);                   // "data.csv"
```

---

## Date and Time

| Function | Description | Returns |
|----------|-------------|---------|
| `now()` | Current Unix timestamp | Seconds since epoch |
| `date(str)` | Legacy date parser | Unix timestamp |
| `date_utc(str)` | Parse `YYYY-MM-DD`, `YYYY-MM-DD HH:MM:SS`, or `YYYY-MM-DDTHH:MM:SSZ` as UTC | Unix timestamp |
| `format_date(ts [, fmt])` | Legacy formatter | String (default RFC 822; custom format uses host local time) |
| `format_date_utc(ts [, fmt])` | Format timestamp in UTC | String (default RFC 822; custom format uses UTC) |

Timezone policy:

- `now()` returns a Unix timestamp and has no timezone.
- `date(str)` uses TurboNet's datetime parser and returns a Unix timestamp.
- `format_date(ts)` without a custom format returns the legacy RFC 822 string.
- `format_date(ts, fmt)` keeps legacy host-local formatting for compatibility.
- Use `date_utc(str)` and `format_date_utc(ts, fmt)` when the script needs deterministic UTC behavior.
- Custom local/UTC formatting is backed by TurboNet platform datetime helpers.
- For native datetime values, use `datetime(text)` or `datetime.parse(text)`.

**Example:**
```javascript
var current = now();                              // 1709568000
var parsed = date_utc("2024-01-01 12:00:00");     // 1704110400
var formatted = format_date_utc(parsed, "%Y-%m-%d %H:%M:%S"); // "2024-01-01 12:00:00"
```

---

## Failure Contract

TurboScript's built-in IO functions currently follow three failure shapes:

- Predicates return `1` or `0`.
  Examples: `file_exists(path)`, `is_file(path)`, `is_dir(path)`, `path_is_absolute(path)`.
- Status/mutation functions return `0` on success and a negative value on failure.
  Examples: `write_file(path, content)`, `append_file(path, content)`, `file_remove(path)`, `file_rename(old, new)`, `mkdir(path)`, `rmdir(path)`, `file_size(path)`.
- Data-producing functions keep the legacy numeric `0` sentinel on failure.
  Examples: `read_file(path)`, `file_stat(path)`, `tmpdir()`, `path_join(base, rel)`, `path_dirname(path)`, `path_basename(path)`, `date(str)`, `date_utc(str)`, `format_date(ts [, fmt])`, `format_date_utc(ts [, fmt])`.

Practical rule:

- if the function answers a yes/no question, expect `0` or `1`
- if the function performs an action, expect `0` on success and `< 0` on failure
- if the function should return string/vector/date data, failure currently appears as numeric `0`

This contract is a compatibility rule for the current DSL. Future strict APIs should prefer explicit `null` or thrown errors instead of numeric sentinels.

---

## Type Conversion

### Type Introspection

| Function | Description | Returns |
|----------|-------------|---------|
| `typeof(x)` | Runtime type name | `"number"`, `"int64"`, `"bool"`, `"bytes"`, `"uuid"`, `"datetime"`, `"date"`, `"time"`, `"duration"`, `"decimal"`, `"string"`, `"vector"`, `"map"`, `"object"`, `"list"`, `"null"`, etc. |
| `is_number(x)` | Check for numeric value (`number` or `int64`) | 1 if numeric, else 0 |
| `is_int64(x)` | Check for 64-bit integer value | 1 if int64, else 0 |
| `is_bool(x)` | Check for boolean value | 1 if bool, else 0 |
| `is_bytes(x)` | Check for byte buffer value | 1 if bytes, else 0 |
| `is_uuid(x)` | Check for UUID value | 1 if uuid, else 0 |
| `is_datetime(x)` | Check for datetime value | 1 if datetime, else 0 |
| `is_date(x)` | Check for date value | 1 if date, else 0 |
| `is_time(x)` | Check for time value | 1 if time, else 0 |
| `is_duration(x)` | Check for duration value | 1 if duration, else 0 |
| `is_decimal(x)` | Check for decimal value | 1 if decimal, else 0 |
| `is_string(x)` | Check for string value | 1 if string, else 0 |
| `is_vector(x)` | Check for numeric vector | 1 if vector, else 0 |
| `is_map(x)` | Check for script `map` | 1 if map, else 0 |
| `is_object(x)` | Check for host/parser/data_bind plain object | 1 if object, else 0 |
| `is_list(x)` | Check for heterogeneous list | 1 if list, else 0 |
| `is_null(x)` | Check for null | 1 if null, else 0 |

Parser and data binding APIs return plain objects for JSON objects, XML query
nodes, schema reflection records, and schema-bound records/unions. Script
`map{...}` literals and TBE `map<K,V>` fields remain `map` values.

| Function | Description | Example |
|----------|-------------|---------|
| `to_num(s)` | String to number | `to_num("123.45")` → `123.45` |
| `to_str(x)` | Number to string | `to_str(42)` → `"42"` |
| `to_int(s)` | String to integer | `to_int("99")` → `99` |
| `to_bool(s)` | String to boolean | `to_bool("true")` → `1` |
| `uuid(text)` | Parse standard UUID text into a native UUID value | `uuid("01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001")` |
| `uuid4()` | Generate a random version 4 UUID | `uuid4()` |
| `uuid7()` | Generate a time-ordered version 7 UUID | `uuid7()` |
| `uuid_string(id)` / `id.to_string()` | Convert UUID to canonical text | `uuid_string(uuid4())` |
| `datetime(text)` / `datetime.parse(text)` | Parse text into a native datetime value | `datetime("Sat, 04 Mar 2006 13:27:54 GMT")` |
| `datetime.to_time(dt)` / `datetime.timestamp(dt)` | Convert datetime or parseable text to Unix epoch seconds | `datetime.to_time(dt)` |
| `datetime_string(dt)` / `dt.to_string()` | Format datetime as RFC822/HTTP GMT text | `datetime_string(dt)` |
| `date.parse(text)` | Parse `YYYY-MM-DD` or parseable datetime text into a native date | `date.parse("2026-06-28")` |
| `date.to_string(d)` / `d.to_string()` | Format a date as `YYYY-MM-DD` | `date.to_string(d)` |
| `time.parse(text)` | Parse `HH:MM[:SS[.mmm]]` into a native time | `time.parse("09:30:05.123")` |
| `time.to_string(t)` / `t.to_string()` | Format a time as `HH:MM:SS[.mmm]` | `time.to_string(t)` |
| `duration.parse(text)` | Parse duration text such as `1h30m5s250ms` into a native duration | `duration.parse("1h30m")` |
| `duration.milliseconds(d)` / `d.milliseconds` | Duration in milliseconds | `duration.milliseconds(span)` |
| `duration.seconds(d)` / `d.seconds` | Duration in seconds | `duration.seconds(span)` |
| `duration.to_string(d)` / `d.to_string()` | Format a duration as `H:MM:SS.mmm` | `duration.to_string(span)` |
| `decimal.parse(text)` | Parse fixed-point decimal text into a native decimal | `decimal.parse("123.45")` |
| `decimal.mantissa(d)` / `d.mantissa` | Decimal mantissa after normalization | `decimal.mantissa(price)` |
| `decimal.scale(d)` / `d.scale` | Decimal scale after normalization | `decimal.scale(price)` |
| `decimal.to_string(d)` / `d.to_string()` | Format decimal as normalized text | `decimal.to_string(price)` |

`datetime.parse`, `datetime.to_time`, and `datetime.format_rfc822` are exported
by the script parser module and use TurboNet's datetime parser/format helpers.
TBE schema scalars `datetime`, `date`, `time`, `duration`, `decimal`, `bigint`,
and `money` are implemented in
`tbe/data_bind`; DataBind uses TurboUtils::Parser directly and binds JSON/XML/CSV
text to native runtime values. Schema emit writes them back as strings.

Schema string fields can also use field formats, for example
`[format(ipaddr)] string ip;` or `[format(url)] string href;`. These validate
JSON/CSV/XML/default text but still bind as normal strings. Supported formats:
`ipaddr`, `ip`, `cidr`, `hostname`, `domain`, `email`, `url`, `uri`,
`macaddr`, `mac`, `semver`, `hex`, `base64`, `base64url`, `currency`,
`json_pointer`, `jsonpath`, `xpath`, `cron`, `color`, `mime`, and `regex`.
`regex` validates patterns through the built-in regex engine.
`schema.fields(...)` exposes the field `format` when present.

---

## Platform Functions

| Function | Description | Returns |
|----------|-------------|---------|
| `os_name()` | Operating system | `"windows"`, `"linux"`, `"macos"`, or `"unknown"` |
| `pid()` | Process ID | Integer |
| `uptime_ms()` | Milliseconds since process start | Integer |
| `monotonic_ms()` | Monotonic clock | Integer (milliseconds) |

**Example:**
```javascript
var os = os_name();
if (os == "windows") {
    print("Running on Windows");
} elif (os == "linux") {
    print("Running on Linux");
}

var process_id = pid();
var uptime = uptime_ms();
print("Process " + process_id + " has been running for " + uptime + "ms");
```

---

## Calculus (Experimental)

| Function | Description | Example |
|----------|-------------|---------|
| `integrate(func_name, a, b, steps)` | Numerical integration | `integrate("f", 0, 3, 1000)` |
| `derivative(func_name, x)` | Numerical derivative | `derivative("f", 2)` |

**Example:**
```javascript
func f(x) {
    return x * x;
}

var area = integrate("f", 0, 3, 1000);  // ∫x² dx from 0 to 3 ≈ 9
var slope = derivative("f", 2);          // f'(2) = 4
```

---

## Module-Specific Functions

For functions provided by optional modules:

- **Parser / CSV / JSON**: See [parser README](../../modules/parser/README.md)
- **CSV filter expressions**: See [csv_filter_expression.md](csv_filter_expression.md)
- **Technical Analysis**: See [ta_fin_cheatsheet.md](ta_fin_cheatsheet.md)
- **Vector Operations**: See [vec_cheatsheet.md](vec_cheatsheet.md)
- **Finance / Strategy**: See [ta_fin_cheatsheet.md](ta_fin_cheatsheet.md) and [modules/FIN_MODULE.md](../modules/FIN_MODULE.md)
- **Network**: See [modules/net.md](modules/net.md)
- **SQLite**: See [modules/sqlite.md](modules/sqlite.md)
- **WebAssembly**: See [../../modules/wasm/README.md](../../modules/wasm/README.md)

---

## See Also

- **[Language Guide](language-guide.md)** - Complete syntax reference
- **[Getting Started](getting-started.md)** - Quick tutorial
- **[Plugin Development](plugin-development.md)** - Extend with C/C++

---

**Complete API documentation for TurboScript**
