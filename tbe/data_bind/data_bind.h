/**
 * @file data_bind.h
 * @brief Public C API for schema-driven binary/JSON/CSV/XML data binding.
 *
 * Third-party users should include this header and link TurboScript::DataBind.
 * The API exposes only opaque handles and accessor functions; returned strings
 * and child pointers are borrowed views owned by their DataBind/DataBindValue.
 *
 * SECURITY: Schema files are assumed to come from trusted sources. DataBind uses
 * JIT compilation to generate binary parsers and includes validation limits to
 * prevent common attacks (max nesting depth: 32, max field offset: 1GB, max total
 * fields: 10,000, circular reference detection). Do not create codecs from
 * user-supplied or untrusted schema definitions.
 */

#ifndef DATA_BIND_H
#define DATA_BIND_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_parser.h"
#include "uuid.h"

#define DATA_BIND_VERSION_MAJOR 1
#define DATA_BIND_VERSION_MINOR 6
#define DATA_BIND_VERSION_PATCH 0
#define DATA_BIND_VERSION \
    (DATA_BIND_VERSION_MAJOR * 10000 + DATA_BIND_VERSION_MINOR * 100 + DATA_BIND_VERSION_PATCH)

/* Increment when the public C ABI changes incompatibly. */
#define DATA_BIND_ABI_VERSION 6

#ifdef __cplusplus
extern "C" {
#endif

/* DLL export/import macros */
#ifdef _WIN32
  #ifdef DATA_BIND_BUILD_DLL
    #define DATA_BIND_API __declspec(dllexport)
  #elif defined(DATA_BIND_USE_DLL)
    #define DATA_BIND_API __declspec(dllimport)
  #else
    #define DATA_BIND_API
  #endif
#else
  #define DATA_BIND_API
#endif

typedef struct DataBind DataBind;
typedef struct DataBindValue DataBindValue;

typedef enum DataBindStatus {
    DATA_BIND_OK = 0,
    DATA_BIND_ERR_INVALID_ARG,
    DATA_BIND_ERR_IO,
    DATA_BIND_ERR_PARSE,
    DATA_BIND_ERR_SCHEMA,
    DATA_BIND_ERR_TYPE_NOT_FOUND,
    DATA_BIND_ERR_TYPE_MISMATCH,
    DATA_BIND_ERR_OOM,
    DATA_BIND_ERR_RUNTIME
} DataBindStatus;

typedef struct DataBindError {
    size_t size;
    DataBindStatus code;
    int line;
    int column;
    char path[260];
    char message[512];
} DataBindError;

typedef int (*DataBindWriteFn)(const void* data, size_t len, void* user);

typedef enum DataBindValueKind {
    DATA_BIND_VALUE_NULL = 0,
    DATA_BIND_VALUE_OBJECT,
    DATA_BIND_VALUE_LIST,
    DATA_BIND_VALUE_SET,
    DATA_BIND_VALUE_MAP,
    DATA_BIND_VALUE_INT,
    DATA_BIND_VALUE_INT64,
    DATA_BIND_VALUE_DOUBLE,
    DATA_BIND_VALUE_BOOL,
    DATA_BIND_VALUE_STRING,
    DATA_BIND_VALUE_BYTES,
    DATA_BIND_VALUE_UUID,
    DATA_BIND_VALUE_DATETIME,
    DATA_BIND_VALUE_DATE,
    DATA_BIND_VALUE_TIME,
    DATA_BIND_VALUE_DURATION,
    DATA_BIND_VALUE_DECIMAL,
    DATA_BIND_VALUE_BIGINT,
    DATA_BIND_VALUE_MONEY
} DataBindValueKind;

typedef struct DataBindDate {
    int year;
    int month;
    int day;
} DataBindDate;

typedef struct DataBindTime {
    int hour;
    int minute;
    int second;
    int millisecond;
} DataBindTime;

typedef struct DataBindDecimal {
    int64_t mantissa;
    int32_t scale;
} DataBindDecimal;

typedef struct DataBindMoney {
    DataBindDecimal amount;
    char currency[4];
} DataBindMoney;

typedef struct DataBindMapEntry {
    const char* key;
    const DataBindValue* value;
} DataBindMapEntry;

typedef enum DataBindSchemaKind {
    DATA_BIND_SCHEMA_UNKNOWN = 0,
    DATA_BIND_SCHEMA_MESSAGE,
    DATA_BIND_SCHEMA_COMPOSITE,
    DATA_BIND_SCHEMA_GROUP,
    DATA_BIND_SCHEMA_ENUM,
    DATA_BIND_SCHEMA_FLAGS,
    DATA_BIND_SCHEMA_UNION,
    DATA_BIND_SCHEMA_SCALAR
} DataBindSchemaKind;

typedef struct DataBindSchemaType {
    size_t size;
    const char* name;
    DataBindSchemaKind kind;
    const char* underlying_type;
    size_t field_count;
    size_t item_count;
    size_t fixed_block_size;
    int has_fixed_block_size;
} DataBindSchemaType;

typedef struct DataBindSchemaField {
    size_t size;
    const char* name;
    const char* type;
    const char* kind;
    const char* inner_type;
    const char* group_type;
    const char* key_type;
    const char* value_type;
    const char* collection_kind;
    const char* length;
    int is_optional;
    int has_default;
    const char* default_value;
    int is_collection;
    int is_composite;
    int is_group;
    int is_map;
    int is_enum;
    int is_variable_size;
    int is_fixed_size;
    size_t offset;
    int has_offset;
    size_t size_bytes;
    int has_size_bytes;
    size_t field_size_bytes;
    int has_field_size_bytes;
    const char* format;
} DataBindSchemaField;

typedef struct DataBindSchemaEnumItem {
    size_t size;
    const char* name;
    const char* value;
} DataBindSchemaEnumItem;

typedef struct DataBindSchemaAttribute {
    size_t size;
    const char* name;
    const char* value;
} DataBindSchemaAttribute;

#define DATA_BIND_SCHEMA_TYPE_INIT      { sizeof(DataBindSchemaType) }
#define DATA_BIND_SCHEMA_FIELD_INIT     { sizeof(DataBindSchemaField) }
#define DATA_BIND_SCHEMA_ENUM_ITEM_INIT { sizeof(DataBindSchemaEnumItem) }
#define DATA_BIND_SCHEMA_ATTRIBUTE_INIT { sizeof(DataBindSchemaAttribute) }
#define DATA_BIND_ERROR_INIT            { sizeof(DataBindError), DATA_BIND_OK, -1, -1, {0}, {0} }

/**
 * @brief Return the linked library version encoded as major * 10000 + minor * 100 + patch.
 */
DATA_BIND_API int data_bind_library_version(void);

/**
 * @brief Return the linked library ABI version.
 */
DATA_BIND_API int data_bind_abi_version(void);

/**
 * @brief Return a stable version string for diagnostics.
 */
DATA_BIND_API const char* data_bind_version_string(void);

/**
 * @brief Create dynamic codec from schema file.
 * @param schema_path Path to .schema file (must be from a trusted source)
 * @param out_codec Output parameter for the created codec
 * @param error Output parameter for error information
 * @return Status code. On success, *out_codec owns the codec and must be freed with data_bind_free().
 *
 * SECURITY: This function uses JIT compilation to generate optimized parsers.
 * Only load schemas from trusted sources. The function validates schema structure
 * (nesting depth, field offsets, circular references) but does not provide
 * comprehensive defense against all malicious constructions.
 */
DATA_BIND_API DataBindStatus data_bind_create(const char* schema_path, DataBind** out_codec,
                                              DataBindError* error);

/**
 * @brief Create dynamic codec from schema text in memory.
 * @param schema_text Schema definition text (must be from a trusted source)
 * @param len Length of schema text
 * @param out_codec Output parameter for the created codec
 * @param error Output parameter for error information
 * @return Status code. On success, *out_codec owns the codec and must be freed with data_bind_free().
 *
 * SECURITY: This function uses JIT compilation. Only process schema text from
 * trusted sources. See data_bind_create() for security considerations.
 */
DATA_BIND_API DataBindStatus data_bind_create_from_text(const char* schema_text, size_t len,
                                                        DataBind** out_codec,
                                                        DataBindError* error);

/**
 * @brief Free codec
 */
DATA_BIND_API void data_bind_free(DataBind* codec);

/**
 * @brief Enable or disable MIR module caching.
 * @param enabled Non-zero to enable caching, zero to disable
 *
 * When enabled (default), data_bind_create() caches compiled MIR modules by
 * schema hash. Multiple codecs with identical schemas share the same JIT-compiled
 * parser, reducing memory usage and codec creation time.
 *
 * Caching is thread-local and does not synchronize across threads. For multi-threaded
 * usage, create codecs on a single thread and distribute them, or disable caching.
 */
DATA_BIND_API void data_bind_set_cache_enabled(int enabled);

/**
 * @brief Clear all cached MIR modules.
 *
 * Releases cached modules that are no longer referenced by any codec. This does not
 * affect existing codecs, only prevents future codecs from reusing cached modules.
 */
DATA_BIND_API void data_bind_clear_cache(void);

/**
 * @brief Enable or disable the value object pool.
 * @param enabled Non-zero to enable pooling, zero to disable
 *
 * When enabled (default), DataBindValue nodes are pooled and reused to reduce
 * allocation overhead. The pool maintains up to 64 free nodes. Disabling the pool
 * causes all allocations to use malloc/free directly.
 *
 * This setting is process-global. For best performance, leave pooling enabled.
 */
DATA_BIND_API void data_bind_set_value_pool_enabled(int enabled);

/**
 * @brief Get value pool statistics.
 * @param allocated Total DataBindValue nodes allocated (output)
 * @param reused Number of times pool nodes were reused (output)
 *
 * Use this to monitor pool effectiveness. High reuse rates indicate good cache locality.
 */
DATA_BIND_API void data_bind_get_value_pool_stats(size_t *allocated, size_t *reused);

/**
 * @brief Parse binary data to a dynamic value tree.
 * @param codec Codec instance
 * @param type_name Message type name (e.g. "Order")
 * @param buf Binary data
 * @param len Data length
 * @return Status code. On success, *out_value owns the value and must be released with data_bind_value_free().
 */
DATA_BIND_API DataBindStatus data_bind_parse(DataBind* codec, const char* type_name,
                                             const uint8_t* buf, size_t len,
                                             DataBindValue** out_value, DataBindError* error);

/**
 * @brief Bind JSON text to a dynamic value tree using the codec schema.
 * @param codec Codec instance
 * @param type_name Schema type name
 * @param json JSON document text
 * @param len JSON text length
 * @return Status code.
 */
DATA_BIND_API DataBindStatus data_bind_parse_json(DataBind* codec, const char* type_name,
                                                  const char* json, size_t len,
                                                  DataBindValue** out_value,
                                                  DataBindError* error);

/**
 * @brief Bind each item in JSON text to a dynamic list using the codec schema.
 *
 * If the input JSON is an array, each item is bound independently. Otherwise
 * the single document is bound and returned as a one-item list.
 */
DATA_BIND_API DataBindStatus data_bind_parse_json_all(DataBind* codec, const char* type_name,
                                                      const char* json, size_t len,
                                                      DataBindValue** out_value,
                                                      DataBindError* error);

/**
 * @brief Bind a CSV row to a dynamic value tree using the codec schema.
 *
 * CSV is parsed with a header row. Nested fields use dotted/indexed column
 * names such as header.seq, bids[0].price, attrs.x.
 */
DATA_BIND_API DataBindStatus data_bind_parse_csv(DataBind* codec, const char* type_name,
                                                 const char* csv, size_t len, size_t row,
                                                 DataBindValue** out_value,
                                                 DataBindError* error);

/**
 * @brief Bind all CSV rows to a dynamic list using the codec schema.
 */
DATA_BIND_API DataBindStatus data_bind_parse_csv_all(DataBind* codec, const char* type_name,
                                                     const char* csv, size_t len,
                                                     DataBindValue** out_value,
                                                     DataBindError* error);

/**
 * @brief Bind XML text to a dynamic value tree using the codec schema.
 *
 * The XML binder uses XPath 1.0 over the parsed document. Record fields bind
 * from same-name child elements first and same-name attributes second.
 * Collections and groups bind from repeated same-name elements.
 */
DATA_BIND_API DataBindStatus data_bind_parse_xml(DataBind* codec, const char* type_name,
                                                 const char* xml, size_t len,
                                                 DataBindValue** out_value,
                                                 DataBindError* error);

/**
 * @brief Bind all XML nodes matched by an XPath expression to a dynamic list.
 *
 * If xpath is NULL or empty, the root document element is bound as a one-item
 * list. The matched nodes are non-owning views into the parsed XML document.
 */
DATA_BIND_API DataBindStatus data_bind_parse_xml_all(DataBind* codec, const char* type_name,
                                                     const char* xml, size_t len,
                                                     const char* xpath,
                                                     DataBindValue** out_value,
                                                     DataBindError* error);

/**
 * @brief Strictly validate JSON text against a schema type.
 *
 * JSON arrays are valid only when every item binds to the requested type.
 * Non-array input is validated as a single value.
 */
DATA_BIND_API DataBindStatus data_bind_validate_json(DataBind* codec, const char* type_name,
                                                     const char* json, size_t len,
                                                     DataBindError* error);

/**
 * @brief Strictly validate every CSV data row against a schema type.
 *
 * CSV is parsed with a header row. Empty inputs with a valid header and no data
 * rows are valid.
 */
DATA_BIND_API DataBindStatus data_bind_validate_csv(DataBind* codec, const char* type_name,
                                                    const char* csv, size_t len,
                                                    DataBindError* error);

/**
 * @brief Strictly validate XML text against a schema type.
 *
 * If xpath is NULL or empty, the document root is validated. Otherwise every
 * XPath-selected node must bind to the requested type.
 */
DATA_BIND_API DataBindStatus data_bind_validate_xml(DataBind* codec, const char* type_name,
                                                    const char* xml, size_t len,
                                                    const char* xpath,
                                                    DataBindError* error);

/**
 * @brief Free a dynamic value tree returned by data_bind_parse().
 *
 * It is valid to pass NULL. All pointers returned from data_bind_value_* accessors
 * become invalid after this call.
 */
DATA_BIND_API void data_bind_value_free(DataBindValue* value);

DATA_BIND_API DataBindValueKind data_bind_value_kind(const DataBindValue* value);
DATA_BIND_API size_t data_bind_value_field_count(const DataBindValue* value);
DATA_BIND_API const char* data_bind_value_field_name(const DataBindValue* value, size_t index);
DATA_BIND_API const DataBindValue* data_bind_value_field_at(const DataBindValue* value,
                                                            size_t index);
DATA_BIND_API const DataBindValue* data_bind_value_get(const DataBindValue* value,
                                                       const char* name);
DATA_BIND_API size_t data_bind_value_count(const DataBindValue* value);
DATA_BIND_API const DataBindValue* data_bind_value_at(const DataBindValue* value, size_t index);
DATA_BIND_API DataBindMapEntry data_bind_value_map_entry_at(const DataBindValue* value,
                                                            size_t index);
DATA_BIND_API int32_t data_bind_value_as_int(const DataBindValue* value);
DATA_BIND_API int64_t data_bind_value_as_int64(const DataBindValue* value);
DATA_BIND_API double data_bind_value_as_double(const DataBindValue* value);
DATA_BIND_API int data_bind_value_as_bool(const DataBindValue* value);
DATA_BIND_API const char* data_bind_value_as_string(const DataBindValue* value);
DATA_BIND_API const uint8_t* data_bind_value_as_bytes(const DataBindValue* value, size_t* len);
DATA_BIND_API int data_bind_value_as_uuid(const DataBindValue* value, uuid_t* out);
DATA_BIND_API const char* data_bind_value_as_uuid_string(const DataBindValue* value,
                                                         char* out, size_t len);
DATA_BIND_API int data_bind_value_as_datetime(const DataBindValue* value, turbo_datetime_t* out);
DATA_BIND_API double data_bind_value_as_datetime_timestamp(const DataBindValue* value);
DATA_BIND_API const char* data_bind_value_as_datetime_string(const DataBindValue* value,
                                                             char* out, size_t len);
DATA_BIND_API int data_bind_value_as_date(const DataBindValue* value, DataBindDate* out);
DATA_BIND_API const char* data_bind_value_as_date_string(const DataBindValue* value,
                                                         char* out, size_t len);
DATA_BIND_API int data_bind_value_as_time(const DataBindValue* value, DataBindTime* out);
DATA_BIND_API const char* data_bind_value_as_time_string(const DataBindValue* value,
                                                         char* out, size_t len);
DATA_BIND_API int64_t data_bind_value_as_duration_milliseconds(const DataBindValue* value);
DATA_BIND_API const char* data_bind_value_as_duration_string(const DataBindValue* value,
                                                             char* out, size_t len);
DATA_BIND_API int data_bind_value_as_decimal(const DataBindValue* value, DataBindDecimal* out);
DATA_BIND_API const char* data_bind_value_as_decimal_string(const DataBindValue* value,
                                                            char* out, size_t len);
DATA_BIND_API const char* data_bind_value_as_bigint_string(const DataBindValue* value);
DATA_BIND_API int data_bind_value_as_money(const DataBindValue* value, DataBindMoney* out);
DATA_BIND_API const char* data_bind_value_as_money_string(const DataBindValue* value,
                                                          char* out, size_t len);
DATA_BIND_API DataBindStatus data_bind_value_get_int32(const DataBindValue* value, int32_t* out);
DATA_BIND_API DataBindStatus data_bind_value_get_int64(const DataBindValue* value, int64_t* out);
DATA_BIND_API DataBindStatus data_bind_value_get_double(const DataBindValue* value, double* out);
DATA_BIND_API DataBindStatus data_bind_value_get_bool(const DataBindValue* value, int* out);
DATA_BIND_API DataBindStatus data_bind_value_get_string(const DataBindValue* value,
                                                        const char** data, size_t* len);
DATA_BIND_API DataBindStatus data_bind_value_get_bytes(const DataBindValue* value,
                                                       const uint8_t** data, size_t* len);
DATA_BIND_API DataBindStatus data_bind_value_get_uuid(const DataBindValue* value, uuid_t* out);
DATA_BIND_API DataBindStatus data_bind_value_get_datetime(const DataBindValue* value,
                                                          turbo_datetime_t* out);
DATA_BIND_API DataBindStatus data_bind_value_get_date(const DataBindValue* value,
                                                      DataBindDate* out);
DATA_BIND_API DataBindStatus data_bind_value_get_time(const DataBindValue* value,
                                                      DataBindTime* out);
DATA_BIND_API DataBindStatus data_bind_value_get_duration_milliseconds(const DataBindValue* value,
                                                                       int64_t* out);
DATA_BIND_API DataBindStatus data_bind_value_get_decimal(const DataBindValue* value,
                                                         DataBindDecimal* out);
DATA_BIND_API DataBindStatus data_bind_value_get_bigint(const DataBindValue* value,
                                                        const char** text,
                                                        size_t* len);
DATA_BIND_API DataBindStatus data_bind_value_get_money(const DataBindValue* value,
                                                       DataBindMoney* out);

/**
 * @brief Return a stable string name for a schema kind.
 */
DATA_BIND_API const char* data_bind_schema_kind_name(DataBindSchemaKind kind);

/**
 * @brief Return the number of reflected schema types.
 *
 * Includes messages, composites, groups, unions, enums and flags. Returned
 * strings from reflection calls are owned by the codec and remain valid until
 * data_bind_free().
 */
DATA_BIND_API size_t data_bind_schema_type_count(DataBind* codec);

/**
 * @brief Read schema type metadata by index.
 * @return 1 when out was filled, 0 when arguments or index are invalid
 */
DATA_BIND_API int data_bind_schema_type_at(DataBind* codec, size_t index,
                                           DataBindSchemaType* out);

/**
 * @brief Find a schema type by name.
 * @return 1 when out was filled, 0 when not found or arguments are invalid
 */
DATA_BIND_API int data_bind_schema_find_type(DataBind* codec, const char* name,
                                             DataBindSchemaType* out);

/**
 * @brief Return the number of fields for a message, composite, group or union.
 */
DATA_BIND_API size_t data_bind_schema_field_count(DataBind* codec, const char* type_name);

/**
 * @brief Read field metadata for a message, composite, group or union.
 * @return 1 when out was filled, 0 when not found or arguments are invalid
 */
DATA_BIND_API int data_bind_schema_field_at(DataBind* codec, const char* type_name,
                                            size_t index, DataBindSchemaField* out);

/**
 * @brief Return the number of enum/flags declarations.
 */
DATA_BIND_API size_t data_bind_schema_enum_count(DataBind* codec);

/**
 * @brief Read enum/flags type metadata by enum index.
 * @return 1 when out was filled, 0 when arguments or index are invalid
 */
DATA_BIND_API int data_bind_schema_enum_at(DataBind* codec, size_t index,
                                           DataBindSchemaType* out);

/**
 * @brief Return the number of items in an enum or flags declaration.
 */
DATA_BIND_API size_t data_bind_schema_enum_item_count(DataBind* codec, const char* enum_name);

/**
 * @brief Read enum/flags item metadata by item index.
 * @return 1 when out was filled, 0 when not found or arguments are invalid
 */
DATA_BIND_API int data_bind_schema_enum_item_at(DataBind* codec, const char* enum_name,
                                                size_t index, DataBindSchemaEnumItem* out);

/**
 * @brief Return the optional schema declaration name.
 */
DATA_BIND_API const char* data_bind_schema_name(DataBind* codec);

/**
 * @brief Return the number of attributes on the schema declaration.
 */
DATA_BIND_API size_t data_bind_schema_attribute_count(DataBind* codec);

/**
 * @brief Read schema declaration attribute metadata by index.
 * @return 1 when out was filled, 0 when not found or arguments are invalid
 */
DATA_BIND_API int data_bind_schema_attribute_at(DataBind* codec, size_t index,
                                                DataBindSchemaAttribute* out);

/**
 * @brief Find a schema declaration attribute by name.
 */
DATA_BIND_API const char* data_bind_schema_attribute_get(DataBind* codec, const char* name);

/**
 * @brief Generate the MIR parser module for a schema without linking or JIT.
 * @param schema_path Path to .schema file
 * @param write Callback receiving textual MIR or binary MIR chunks
 * @param user Caller context passed to write
 * @param binary_output Non-zero writes binary MIR, zero writes textual MIR
 * @return DATA_BIND_OK on success, otherwise a DataBindStatus error code
 */
DATA_BIND_API DataBindStatus data_bind_generate_mir(const char* schema_path,
                                                    DataBindWriteFn write,
                                                    void* user,
                                                    int binary_output,
                                                    DataBindError* error);

DATA_BIND_API const char* data_bind_status_name(DataBindStatus status);

#ifdef __cplusplus
}
#endif

#endif /* DATA_BIND_H */
