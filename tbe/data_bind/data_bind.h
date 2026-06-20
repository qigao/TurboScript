/**
 * @file data_bind.h
 * @brief MIR-based runtime binary codec (zero compilation)
 */

#ifndef DATA_BIND_H
#define DATA_BIND_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
typedef struct Value Value;

/**
 * @brief Value API function pointers (provided by host application)
 */
typedef struct DataBindValueApi {
    Value* (*create_object)(void);
    void   (*set_field_int)(Value* obj, const char* name, int32_t val);
    void   (*set_field_int64)(Value* obj, const char* name, int64_t val);
    void   (*set_field_double)(Value* obj, const char* name, double val);
    void   (*set_field_string)(Value* obj, const char* name, const char* val);
    void   (*set_field_bytes)(Value* obj, const char* name, const uint8_t* data, size_t len);
    Value* (*create_list)(void);
    void   (*add_list_item_int)(Value* list, int32_t val);
    void   (*add_list_item_int64)(Value* list, int64_t val);
    void   (*add_list_item_double)(Value* list, double val);
    void   (*add_list_item_string)(Value* list, const char* val);
    void   (*add_list_item_object)(Value* list, Value* obj);
    void   (*set_field_list)(Value* obj, const char* name, Value* list);
    void   (*set_field_object)(Value* obj, const char* name, Value* child);
    Value* (*create_set)(void);
    void   (*add_set_item_int)(Value* set, int32_t val);
    void   (*add_set_item_double)(Value* set, double val);
    void   (*add_set_item_string)(Value* set, const char* val);
    void   (*set_field_set)(Value* obj, const char* name, Value* set);
    Value* (*create_map)(void);
    void   (*add_map_entry_string_string)(Value* map, const char* key, const char* val);
    void   (*add_map_entry_string_int)(Value* map, const char* key, int32_t val);
    void   (*add_map_entry_string_double)(Value* map, const char* key, double val);
    void   (*set_field_map)(Value* obj, const char* name, Value* map);
} DataBindValueApi;

/**
 * @brief Create codec from schema file
 * @param schema_path Path to .schema file
 * @param api Value API function pointers (must remain valid for codec lifetime)
 * @return Codec instance, or NULL on failure
 */
DATA_BIND_API DataBind* data_bind_create(const char* schema_path, const DataBindValueApi* api);

/**
 * @brief Free codec
 */
DATA_BIND_API void data_bind_free(DataBind* codec);

/**
 * @brief Parse binary data to Value object
 * @param codec Codec instance
 * @param type_name Message type name (e.g. "Order")
 * @param buf Binary data
 * @param len Data length
 * @return Value object, or NULL on failure
 */
DATA_BIND_API Value* data_bind_parse(DataBind* codec, const char* type_name,
                        const uint8_t* buf, size_t len);

/**
 * @brief Generate the MIR parser module for a schema without linking or JIT.
 * @param schema_path Path to .schema file
 * @param out Output stream receiving textual MIR or binary MIR
 * @param binary_output Non-zero writes binary MIR, zero writes textual MIR
 * @param error_buf Optional error buffer
 * @param error_size Size of error_buf
 * @return 0 on success, non-zero on failure
 */
DATA_BIND_API int data_bind_generate_mir(const char* schema_path, FILE* out,
                                         int binary_output, char* error_buf,
                                         size_t error_size);

/**
 * @brief Get last error message
 */
DATA_BIND_API const char* data_bind_get_error(DataBind* codec);

#ifdef __cplusplus
}
#endif

#endif /* DATA_BIND_H */
