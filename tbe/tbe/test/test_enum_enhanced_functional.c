/**
 * @file test_enum_enhanced_functional.c
 * @brief Functional tests for enum-enhanced generated code using tinytest BDD style.
 */

#include <stdint.h>
#include <string.h>
#include "tinytest.h"

#include "test_enum_enhanced.h"

suite("enum_enhanced_functional") {
    describe("UserRole Enum Helpers") {
        it("should return correct count") {
            check_uint_eq(UserRole_count(), 4);
        }

        it("should return correct min/max") {
            check_int_eq(UserRole_min(), UserRole_Guest);
            check_int_eq(UserRole_max(), UserRole_Admin);
        }

        it("should convert to string") {
            check_str_eq(UserRole_to_string(UserRole_Guest), "Guest");
            check_str_eq(UserRole_to_string(UserRole_Admin), "Admin");
            check_null(UserRole_to_string((UserRole_t)999));
        }

        it("should convert from string") {
            UserRole_t role;
            check(UserRole_from_string("User", &role));
            check_int_eq(role, UserRole_User);

            check(!UserRole_from_string("Invalid", &role));
        }

        it("should validate values") {
            check(UserRole_is_valid(UserRole_Moderator));
            check(!UserRole_is_valid((UserRole_t)999));
        }
    }

    describe("Status Enum Helpers") {
        it("should return correct count") {
            check_uint_eq(Status_count(), 4);
        }

        it("should return correct min/max") {
            check_int_eq(Status_min(), Status_Inactive);
            check_int_eq(Status_max(), Status_Deleted);
        }

        it("should convert to/from string") {
            check_str_eq(Status_to_string(Status_Active), "Active");
            check_str_eq(Status_to_string(Status_Suspended), "Suspended");

            Status_t s;
            check(Status_from_string("Deleted", &s));
            check_int_eq(s, Status_Deleted);
            check(!Status_from_string("UnknownStatus", &s));
        }

        it("should validate values") {
            check(Status_is_valid(Status_Active));
            check(!Status_is_valid((Status_t)100));
        }
    }

    describe("UserProfile Wire Integration") {
        it("should bind builder and set required fields") {
            uint8_t buffer[256] = {0};
            UserProfile_builder_t builder;
            check(UserProfile_builder_bind(&builder, buffer, sizeof(buffer)));
            check(UserProfile_id_set(&builder, 12345));
            check(UserProfile_status_set(&builder, Status_Active));
        }

        it("should set optional role with enum validation") {
            uint8_t buffer[256] = {0};
            UserProfile_builder_t builder;
            UserProfile_view_t view;

            check(UserProfile_builder_bind(&builder, buffer, sizeof(buffer)));
            check(UserProfile_id_set(&builder, 12345));
            check(UserProfile_status_set(&builder, Status_Active));

            UserRole_t role = UserRole_Admin;
            check(UserRole_is_valid(role));
            check(UserProfile_role_set(&builder, role));
            UserProfile_set_optional_field(&builder, UserProfile_OPTIONAL_role);

            check(UserProfile_view_bind(&view, buffer, sizeof(buffer)));
            check_int_eq(UserProfile_id_get(&view), 12345);
            check_int_eq(UserProfile_status_get(&view), Status_Active);
            check_int_eq(UserProfile_role_get(&view), UserRole_Admin);
            check(UserProfile_has_role(&view));
        }

        it("should read back variable-length username field") {
            uint8_t buffer[256] = {0};
            UserProfile_builder_t builder;
            UserProfile_view_t view;
            const char *username = "admin_user";

            check(UserProfile_builder_bind(&builder, buffer, sizeof(buffer)));
            check(UserProfile_id_set(&builder, 99));
            check(UserProfile_status_set(&builder, Status_Active));
            check(UserProfile_username_set(&builder, username, strlen(username)));
            UserProfile_set_optional_field(&builder, UserProfile_OPTIONAL_username);

            check(UserProfile_view_bind(&view, buffer, sizeof(buffer)));
            check(UserProfile_has_username(&view));

            tbe_var_data_t ud;
            check(UserProfile_username(&view, &ud));
            check_uint_eq(ud.size, strlen(username));
            check(memcmp(ud.data, username, ud.size) == 0);
        }
    }
}
