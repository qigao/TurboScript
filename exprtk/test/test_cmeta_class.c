#include "exprtk_class.h"
#include "exprtk_module.h"
#include "tinytest.h"

#include <string.h>

static exprtk_value_t null_value(void) {
    exprtk_value_t value = {0};
    value.type = EXPRTK_VAL_NULL;
    return value;
}

spec("CMeta class reflection") {
    it("publishes a deterministic semantic type identity across clones") {
        mem_pool_t source_arena = {0};
        mem_pool_t clone_arena = {0};
        exprtk_class_t *source;
        exprtk_class_t *clone;
        const cmeta_type_desc *source_type;
        const cmeta_type_desc *clone_type;

        check((mem_init(&source_arena, 4096)) == (0));
        check((mem_init(&clone_arena, 4096)) == (0));

        source = exprtk_class_create(&source_arena, "Point", NULL, NULL, 0);
        check_not_null(source);
        check(exprtk_class_declare_instance_field_typed(
            source, "x", "int64", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_declare_instance_field_typed(
            source, "y", "number", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_finalize_cmeta(source));

        source_type = exprtk_class_cmeta_type(source);
        check_not_null(source_type);
        check(cmeta_type_desc_valid(source_type));
        check_not_null(source_type->identity);
        check_not_null(source_type->identity->stable_atom_id);

        clone = exprtk_class_clone_to_arena(source, &clone_arena);
        check_not_null(clone);
        clone_type = exprtk_class_cmeta_type(clone);
        check_not_null(clone_type);
        check(cmeta_type_equal(source_type, clone_type));
        check(strcmp(source_type->identity->stable_atom_id,
                     clone_type->identity->stable_atom_id) == 0);
        check(source_type != clone_type);

        exprtk_class_destroy(clone);
        exprtk_class_destroy(source);
        mem_destroy(&clone_arena);
        mem_destroy(&source_arena);
    }

    it("binds typed script fields through the canonical CMeta object facade") {
        mem_pool_t arena = {0};
        exprtk_class_t *klass;
        exprtk_instance_t *instance;
        cmeta_object_ref object = CMETA_OBJECT_REF_INIT;
        const cmeta_data_desc *data;
        const void *value = NULL;
        int64_t id = INT64_C(37);
        double score = 3.5;
        bool active = true;
        exprtk_value_t name = exprtk_val_str(vstr_from_cstr("Ada"));

        check((mem_init(&arena, 8192)) == (0));
        klass = exprtk_class_create(&arena, "User", NULL, NULL, 0);
        check_not_null(klass);
        check(exprtk_class_declare_instance_field_typed(
            klass, "id", "int64", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_declare_instance_field_typed(
            klass, "score", "number", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_declare_instance_field_typed(
            klass, "active", "bool", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_declare_instance_field_typed(
            klass, "name", "string", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_finalize_cmeta_data(klass));

        data = exprtk_class_cmeta_data(klass);
        check_not_null(data);
        check(cmeta_data_desc_valid(data));
        check_equal(data->kind, CMETA_DATA_STRUCT);
        check_not_null(data->shape);
        check_equal(((const cmeta_data_struct_shape *)data->shape)->field_count, 4u);
        for (size_t i = 0; i < 4u; ++i) {
            check_equal(klass->cmeta_data_fields[i].offset,
                        CMETA_FIELD_DYNAMIC_OFFSET);
            check_equal(klass->cmeta_layout_fields[i].offset,
                        CMETA_FIELD_DYNAMIC_OFFSET);
        }

        instance = exprtk_instance_create(klass, &arena);
        check_not_null(instance);
        check_equal(exprtk_instance_borrow_cmeta_object(instance, &object),
                    CMETA_OK);
        check(cmeta_object_ref_valid(&object));

        check_equal(cmeta_object_field_assign(
                        &object, "id", klass->cmeta_data_fields[0].value, &id),
                    CMETA_OK);
        check_equal(cmeta_object_field_assign(
                        &object, "score", klass->cmeta_data_fields[1].value,
                        &score),
                    CMETA_OK);
        check_equal(cmeta_object_field_assign(
                        &object, "active", klass->cmeta_data_fields[2].value,
                        &active),
                    CMETA_OK);
        check_equal(cmeta_object_field_assign(
                        &object, "name", klass->cmeta_data_fields[3].value,
                        &name),
                    CMETA_OK);

        check_equal(cmeta_object_field_read(&object, "id", &data, &value),
                    CMETA_OK);
        check_true(data == &cmeta_data_int64);
        check_equal(*(const int64_t *)value, INT64_C(37));

        check_equal(cmeta_object_field_read(&object, "score", &data, &value),
                    CMETA_OK);
        check_true(data == &cmeta_data_double);
        check_true(*(const double *)value == 3.5);

        check_equal(cmeta_object_field_read(&object, "active", &data, &value),
                    CMETA_OK);
        check_true(data == &cmeta_data_bool);
        check_true(*(const bool *)value);

        check_equal(cmeta_object_field_read(&object, "name", &data, &value),
                    CMETA_OK);
        check_equal(data->kind, CMETA_DATA_STRING);
        {
            const unsigned char *bytes = NULL;
            size_t length = 0u;
            check_equal(cmeta_data_buffer_read(
                            data, value, SIZE_MAX, &bytes, &length),
                        CMETA_OK);
            check_equal(length, 3u);
            check_true(bytes != NULL);
            check_equal(memcmp(bytes, "Ada", 3u), 0);
        }

        cmeta_object_release(&object);
        exprtk_instance_destroy(instance);
        exprtk_class_destroy(klass);
        mem_destroy(&arena);
    }

    it("distinguishes classes with different field type shapes") {
        mem_pool_t left_arena = {0};
        mem_pool_t right_arena = {0};
        exprtk_class_t *left;
        exprtk_class_t *right;

        check((mem_init(&left_arena, 4096)) == (0));
        check((mem_init(&right_arena, 4096)) == (0));

        left = exprtk_class_create(&left_arena, "Box", NULL, NULL, 0);
        right = exprtk_class_create(&right_arena, "Box", NULL, NULL, 0);
        check_not_null(left);
        check_not_null(right);

        check(exprtk_class_declare_instance_field_typed(
            left, "value", "number", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_declare_instance_field_typed(
            right, "value", "string", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_finalize_cmeta(left));
        check(exprtk_class_finalize_cmeta(right));

        check_false(cmeta_type_equal(exprtk_class_cmeta_type(left),
                                     exprtk_class_cmeta_type(right)));

        exprtk_class_destroy(right);
        exprtk_class_destroy(left);
        mem_destroy(&right_arena);
        mem_destroy(&left_arena);
    }

    it("includes inheritance in semantic identity and preserves it in clones") {
        mem_pool_t source_arena = {0};
        mem_pool_t clone_arena = {0};
        exprtk_class_t *base;
        exprtk_class_t *child;
        exprtk_class_t *clone;

        check((mem_init(&source_arena, 8192)) == (0));
        check((mem_init(&clone_arena, 8192)) == (0));

        base = exprtk_class_create(&source_arena, "Base", NULL, NULL, 0);
        child = exprtk_class_create(&source_arena, "Child", NULL, NULL, 0);
        check_not_null(base);
        check_not_null(child);
        check(exprtk_class_declare_instance_field_typed(
            base, "id", "int64", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_finalize_cmeta(base));

        exprtk_class_set_prototype(child, base);
        check(exprtk_class_declare_instance_field_typed(
            child, "name", "string", null_value(), 0, EXPRTK_ACCESS_PUBLIC));
        check(exprtk_class_finalize_cmeta(child));

        check_false(cmeta_type_equal(exprtk_class_cmeta_type(base),
                                     exprtk_class_cmeta_type(child)));

        clone = exprtk_class_clone_to_arena(child, &clone_arena);
        check_not_null(clone);
        check(cmeta_type_equal(exprtk_class_cmeta_type(child),
                               exprtk_class_cmeta_type(clone)));

        if (clone->prototype) exprtk_class_destroy(clone->prototype);
        exprtk_class_destroy(clone);
        exprtk_class_destroy(child);
        exprtk_class_destroy(base);
        mem_destroy(&clone_arena);
        mem_destroy(&source_arena);
    }
}
