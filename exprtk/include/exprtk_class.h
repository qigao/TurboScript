/**
 * @file exprtk_class.h
 * @brief TurboScript OOP - Class and Instance Runtime Support
 * 
 * This file defines the runtime structures for TurboScript's object-oriented
 * programming features, including:
 * - Class definitions with methods and constructors
 * - Instance objects with fields
 * - Prototype-based inheritance
 * - Method lookup and binding
 */

#ifndef EXPRTK_CLASS_H
#define EXPRTK_CLASS_H

#include "exprtk_types.h"
#include "turbo_buffer.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct exprtk_class_s exprtk_class_t;
typedef struct exprtk_instance_s exprtk_instance_t;
typedef struct exprtk_method_entry_s exprtk_method_entry_t;

/**
 * @brief Method entry - maps method name to function
 */
struct exprtk_method_entry_s {
    char *name;
    exprtk_func_t *func;
};

/**
 * @brief Class object - represents a class definition
 * 
 * A class contains:
 * - Constructor function
 * - Instance methods (shared by all instances)
 * - Static methods (called on class, not instance)
 * - Prototype link (for inheritance)
 */
struct exprtk_class_s {
    char *name;                      /* Class name */
    exprtk_func_t *constructor;      /* Constructor function (optional) */
    void *constructors;              /* HTAB(exprtk_method_entry_t)* - overloaded constructors */
    void *methods;                   /* HTAB(exprtk_method_entry_t)* - instance methods */
    void *static_methods;            /* HTAB(exprtk_method_entry_t)* - static methods */
    void *static_fields;             /* HTAB(field_entry_t)* - static fields */
    void *abstract_methods;          /* HTAB(exprtk_method_entry_t)* - unresolved abstract methods */
    char **instance_field_names;      /* Canonical instance field layout for slot access */
    char **instance_field_types;      /* NULL for legacy dynamic fields */
    int *instance_field_access_levels;
    struct exprtk_class_s **instance_field_owners;
    exprtk_value_t *instance_field_defaults;
    unsigned char *instance_field_has_default;
    size_t instance_field_count;
    size_t instance_field_capacity;
    struct exprtk_class_s **interfaces; /* Direct implemented/extended interfaces */
    size_t interface_count;
    size_t interface_capacity;
    struct exprtk_class_s *prototype; /* Parent class (NULL for base class) */
    uint64_t type_id;                  /* Stable identity preserved across class clones */
    uint64_t static_field_version;    /* Bumped when the static field table shape changes */
    int is_abstract;                 /* Whether this class cannot be instantiated */
    int is_interface;                /* Whether this class is an interface constraint */
    int is_final;                    /* Whether this class cannot be extended */
    mem_pool_t *arena;               /* Memory arena for class metadata */
};

/**
 * @brief Instance object - represents a class instance
 * 
 * An instance contains:
 * - Reference to its class
 * - Instance fields (key-value pairs)
 */
struct exprtk_instance_s {
    exprtk_class_t *klass;  /* Pointer to class definition */
    void *fields;           /* HTAB(exprtk_map_kv_t)* - instance fields (this.x, this.y, ...) */
    exprtk_value_t *field_slots;      /* Field values indexed by klass->instance_field_names */
    unsigned char *field_slot_used;   /* Whether the corresponding slot has been assigned */
    size_t field_slot_count;
    size_t field_slot_capacity;
    uint64_t field_version;  /* Bumped when the instance field table shape changes */
    mem_pool_t *arena;      /* Memory arena for instance data */
};

/**
 * @brief Bound method - method with bound 'this' context
 * 
 * When accessing a method on an instance (e.g., obj.method), we return
 * a bound method that captures both the function and the instance.
 */
typedef struct {
    exprtk_instance_t *instance;  /* The 'this' object */
    exprtk_func_t *method;        /* The method function */
} exprtk_bound_method_t;

/* ========================================================================
 * Class Management API
 * ======================================================================== */

/**
 * @brief Create a new class
 * @param arena Memory arena for class metadata
 * @param name Class name (copied)
 * @param constructor_node Constructor AST node (optional, can be NULL)
 * @param method_nodes Array of method AST nodes
 * @param method_count Number of methods
 * @return New class object, or NULL on failure
 */
CXX_C_API exprtk_class_t *exprtk_class_create(
    mem_pool_t *arena,
    const char *name,
    exprtk_node_t *constructor_node,
    exprtk_node_t **method_nodes,
    size_t method_count
);

/**
 * @brief Clone class metadata into another arena
 * @param klass Source class
 * @param arena Destination arena
 * @return Class metadata owned by arena, or NULL on failure
 */
CXX_C_API exprtk_class_t *exprtk_class_clone_to_arena(
    exprtk_class_t *klass,
    mem_pool_t *arena
);

/**
 * @brief Set parent class (for inheritance)
 * @param klass Child class
 * @param parent Parent class
 */
CXX_C_API void exprtk_class_set_prototype(exprtk_class_t *klass, exprtk_class_t *parent);

/**
 * @brief Mark a class as explicitly abstract
 * @param klass The class
 * @param is_abstract Non-zero if abstract
 */
CXX_C_API void exprtk_class_set_abstract(exprtk_class_t *klass, int is_abstract);

CXX_C_API void exprtk_class_set_interface(exprtk_class_t *klass, int is_interface);

CXX_C_API void exprtk_class_set_final(exprtk_class_t *klass, int is_final);

CXX_C_API int exprtk_class_is_final(exprtk_class_t *klass);

CXX_C_API void exprtk_class_add_interface(exprtk_class_t *klass,
                                          exprtk_class_t *interface_class);

CXX_C_API int exprtk_class_is_a(exprtk_class_t *klass, exprtk_class_t *target);

/**
 * @brief Add a method to a class
 * @param klass The class
 * @param name Method name
 * @param method_func Method function
 * @param is_static Whether this is a static method
 */
CXX_C_API void exprtk_class_add_method(
    exprtk_class_t *klass,
    const char *name,
    exprtk_func_t *method_func,
    int is_static
);

CXX_C_API void exprtk_class_add_constructor(
    exprtk_class_t *klass,
    exprtk_func_t *constructor_func
);

CXX_C_API exprtk_func_t *exprtk_class_lookup_constructor_typed(
    exprtk_class_t *klass,
    size_t argc,
    const exprtk_value_t *args
);

/**
 * @brief Add an unresolved abstract method requirement to a class
 * @param klass The class
 * @param name Method name
 */
CXX_C_API void exprtk_class_add_abstract_method(
    exprtk_class_t *klass,
    const char *name
);

CXX_C_API void exprtk_class_add_abstract_method_arity(
    exprtk_class_t *klass,
    const char *name,
    size_t argc
);

CXX_C_API void exprtk_class_add_abstract_method_signature(
    exprtk_class_t *klass,
    const char *name,
    exprtk_node_t **arg_params,
    size_t argc,
    exprtk_env_t *closure_env,
    int is_static_method
);

CXX_C_API void exprtk_class_add_abstract_methods_from(
    exprtk_class_t *klass,
    exprtk_class_t *interface_class
);

/**
 * @brief Lookup a method (searches prototype chain)
 * @param klass The class to start search from
 * @param name Method name
 * @param is_static Whether to search static methods
 * @return Method function, or NULL if not found
 */
CXX_C_API exprtk_func_t *exprtk_class_lookup_method(
    exprtk_class_t *klass,
    const char *name,
    int is_static
);

CXX_C_API exprtk_func_t *exprtk_class_lookup_method_arity(
    exprtk_class_t *klass,
    const char *name,
    int is_static,
    size_t argc
);

CXX_C_API exprtk_func_t *exprtk_class_lookup_method_signature(
    exprtk_class_t *klass,
    exprtk_func_t *signature
);

CXX_C_API exprtk_func_t *exprtk_class_lookup_method_typed(
    exprtk_class_t *klass,
    const char *name,
    int is_static,
    size_t argc,
    const exprtk_value_t *args
);

/**
 * @brief Get a static field value from a class (searches prototype chain)
 * @param klass The class
 * @param name Field name
 * @param out_value Output parameter for the value
 * @return 1 if field exists, 0 if not found
 */
CXX_C_API int exprtk_class_get_static_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t *out_value
);

CXX_C_API exprtk_value_t *exprtk_class_get_static_field_slot(
    exprtk_class_t *klass,
    const char *name,
    exprtk_class_t **owner_class
);

/**
 * @brief Set a static field value on a class
 * @param klass The class
 * @param name Field name (copied if new)
 * @param value Field value
 */
CXX_C_API void exprtk_class_set_static_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t value
);

CXX_C_API void exprtk_class_declare_static_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t value,
    int access_level
);

CXX_C_API int exprtk_class_declare_static_field_typed(
    exprtk_class_t *klass,
    const char *name,
    const char *declared_type,
    exprtk_value_t value,
    int access_level
);

CXX_C_API const char *exprtk_class_get_static_field_type(
    exprtk_class_t *klass,
    const char *name
);

CXX_C_API int exprtk_class_set_static_field_checked(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t value,
    char *error_msg,
    size_t error_msg_len
);

CXX_C_API int exprtk_class_get_static_field_access(
    exprtk_class_t *klass,
    const char *name,
    exprtk_class_t **owner_class
);

CXX_C_API int exprtk_class_declare_instance_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t default_value,
    int has_default,
    int access_level
);

CXX_C_API int exprtk_class_declare_instance_field_typed(
    exprtk_class_t *klass,
    const char *name,
    const char *declared_type,
    exprtk_value_t default_value,
    int has_default,
    int access_level
);

CXX_C_API const char *exprtk_class_get_instance_field_type(
    exprtk_class_t *klass,
    const char *name
);

CXX_C_API int exprtk_class_has_instance_field(
    exprtk_class_t *klass,
    const char *name
);

CXX_C_API int exprtk_class_get_instance_field_access(
    exprtk_class_t *klass,
    const char *name,
    exprtk_class_t **owner_class
);

/**
 * @brief Finalize abstract method requirements after methods/prototype are set
 * @param klass The class
 */
CXX_C_API void exprtk_class_finalize_abstract_methods(exprtk_class_t *klass);

/**
 * @brief Check whether a class is abstract
 * @param klass The class
 * @return Non-zero if the class cannot be instantiated
 */
CXX_C_API int exprtk_class_is_abstract(exprtk_class_t *klass);

CXX_C_API int exprtk_class_is_interface(exprtk_class_t *klass);

/**
 * @brief Free class resources
 * @param klass Class to free
 */
CXX_C_API void exprtk_class_destroy(exprtk_class_t *klass);

/* ========================================================================
 * Instance Management API
 * ======================================================================== */

/**
 * @brief Create a new instance of a class
 * @param klass The class to instantiate
 * @param arena Memory arena for instance data
 * @return New instance object, or NULL on failure
 */
CXX_C_API exprtk_instance_t *exprtk_instance_create(
    exprtk_class_t *klass,
    mem_pool_t *arena
);

/**
 * @brief Get a field value from an instance
 * @param instance The instance
 * @param name Field name
 * @param out_value Output parameter for the value
 * @return 1 if field exists, 0 if not found
 */
CXX_C_API int exprtk_instance_get_field(
    exprtk_instance_t *instance,
    const char *name,
    exprtk_value_t *out_value
);

CXX_C_API exprtk_value_t *exprtk_instance_get_field_slot(
    exprtk_instance_t *instance,
    const char *name
);

/**
 * @brief Set a field value on an instance
 * @param instance The instance
 * @param name Field name (copied if new)
 * @param value Field value
 */
CXX_C_API void exprtk_instance_set_field(
    exprtk_instance_t *instance,
    const char *name,
    exprtk_value_t value
);

CXX_C_API int exprtk_instance_set_field_checked(
    exprtk_instance_t *instance,
    const char *name,
    exprtk_value_t value,
    char *error_msg,
    size_t error_msg_len
);

typedef void (*exprtk_instance_field_visitor_t)(exprtk_value_t *value, void *user_data);
typedef void (*exprtk_instance_named_field_visitor_t)(const char *name,
                                                       exprtk_value_t *value,
                                                       void *user_data);

/**
 * @brief Visit all field values stored on an instance
 * @param instance The instance
 * @param visitor Callback invoked for each field value
 * @param user_data Opaque callback data
 */
CXX_C_API void exprtk_instance_foreach_field(
    exprtk_instance_t *instance,
    exprtk_instance_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Visit all named field values stored on an instance
 * @param instance The instance
 * @param visitor Callback invoked for each field name and value
 * @param user_data Opaque callback data
 */
CXX_C_API void exprtk_instance_foreach_named_field(
    exprtk_instance_t *instance,
    exprtk_instance_named_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Visit all static field values stored directly on a class
 * @param klass The class
 * @param visitor Callback invoked for each field value
 * @param user_data Opaque callback data
 */
CXX_C_API void exprtk_class_foreach_static_field(
    exprtk_class_t *klass,
    exprtk_instance_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Visit all named static field values stored directly on a class
 * @param klass The class
 * @param visitor Callback invoked for each field name and value
 * @param user_data Opaque callback data
 */
CXX_C_API void exprtk_class_foreach_named_static_field(
    exprtk_class_t *klass,
    exprtk_instance_named_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Get a method from an instance (searches class and prototype chain)
 * @param instance The instance
 * @param name Method name
 * @return Method function, or NULL if not found
 */
CXX_C_API exprtk_func_t *exprtk_instance_get_method(
    exprtk_instance_t *instance,
    const char *name
);

CXX_C_API exprtk_func_t *exprtk_instance_get_method_arity(
    exprtk_instance_t *instance,
    const char *name,
    size_t argc
);

/**
 * @brief Check if an instance is an instance of a class (including inheritance)
 * @param instance The instance
 * @param klass The class to check against
 * @return 1 if instance is of type klass, 0 otherwise
 */
CXX_C_API int exprtk_instance_of(
    exprtk_instance_t *instance,
    exprtk_class_t *klass
);

/**
 * @brief Free instance resources
 * @param instance Instance to free
 */
CXX_C_API void exprtk_instance_destroy(exprtk_instance_t *instance);

/* ========================================================================
 * Value Constructors
 * ======================================================================== */

/**
 * @brief Create a class value
 * @param klass The class
 * @return exprtk_value_t of type EXPRTK_VAL_CLASS
 */
static inline exprtk_value_t exprtk_val_class(exprtk_class_t *klass) {
    exprtk_value_t v = {0};
    v.type = EXPRTK_VAL_CLASS;
    v.data.class_val.klass = klass;
    return v;
}

/**
 * @brief Create an instance value
 * @param instance The instance
 * @return exprtk_value_t of type EXPRTK_VAL_INSTANCE
 */
static inline exprtk_value_t exprtk_val_instance(exprtk_instance_t *instance) {
    exprtk_value_t v = {0};
    v.type = EXPRTK_VAL_INSTANCE;
    v.data.instance_val.instance = instance;
    return v;
}

/**
 * @brief Create a bound method value
 * @param instance The 'this' object
 * @param method The method function
 * @return exprtk_value_t of type EXPRTK_VAL_BOUND_METHOD
 */
static inline exprtk_value_t exprtk_val_bound_method(exprtk_instance_t *instance, 
                                                       exprtk_func_t *method) {
    exprtk_value_t v = {0};
    v.type = EXPRTK_VAL_BOUND_METHOD;
    v.data.bound_method_val.instance = instance;
    v.data.bound_method_val.method = method;
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* EXPRTK_CLASS_H */

