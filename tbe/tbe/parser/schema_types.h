/**
 * @file schema_types.h
 * @brief Schema Parser Internal Types (shared between grammar and driver)
 */

#ifndef SCHEMA_TYPES_H
#define SCHEMA_TYPES_H

#include "node_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCHEMA_RECORD_COMPOSITE = 0,
    SCHEMA_RECORD_GROUP,
    SCHEMA_RECORD_MESSAGE,
    SCHEMA_RECORD_UNION
} schema_record_kind_t;

typedef enum {
    SCHEMA_FIELD_SECTION_FIXED = 0,
    SCHEMA_FIELD_SECTION_GROUP,
    SCHEMA_FIELD_SECTION_VAR_DATA
} schema_field_section_t;

typedef struct {
    Node *root;           /**< the temporary root map being built              */
    Node *schema_node;    /**< optional schema metadata map                    */
    Node *messages_list;  /**< the "messages" list hanging off root            */
    Node *composites_list;/**< the "composites" list hanging off root         */
    Node *groups_list;    /**< the "groups" list hanging off root              */
    Node *enums_list;     /**< the "enums" list hanging off root               */
    Node *unions_list;    /**< the "unions" list hanging off root              */
    Node *cur_record;     /**< current record-like map being built             */
    Node *cur_fields;     /**< current "fields" list being built               */
    Node *cur_enum;       /**< current enum map being built                    */
    Node *cur_enum_items; /**< current enum items list being built             */
    schema_record_kind_t cur_record_kind; /**< current declaration kind       */
    schema_field_section_t cur_field_section; /**< current field ordering state */
    unsigned long long next_enum_value; /**< next implicit enum value        */
    int   error;
    int   error_line;     /**< line number of last error                       */
    int   error_column;   /**< column number of last error                     */
    char  error_msg[256]; /**< error message buffer                            */
} schema_parse_ctx_t;


#ifdef __cplusplus
}
#endif

#endif /* SCHEMA_TYPES_H */
