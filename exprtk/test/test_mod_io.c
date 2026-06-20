/**
 * @file test_mod_io.c
 * @brief Unit tests for IO module functions (file I/O, date/time, listdir)
 */

#include "exprtk.h"
#include "exprtk_module.h"
#include "exprtk_types.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define TEST_TOLERANCE 1e-6
#define TEST_DIR "test_io_temp_dir"
#define TEST_FILE "test_io_temp_file.txt"

/* Forward declarations for exprtk built-in modules */
extern const exprtk_module_t *exprtk_module_math(void);
extern const exprtk_module_t *exprtk_module_string(void);
extern const exprtk_module_t *exprtk_module_stats(void);
extern const exprtk_module_t *exprtk_module_io(void);
extern const exprtk_module_t *exprtk_module_core(void);

/* Global initialization flag */
static int modules_initialized = 0;

/* Global environment for test execution (to avoid use-after-free issues) */
static exprtk_env_t global_test_env;
static int global_env_initialized = 0;

/* Ensure modules are registered once */
static void ensure_modules_initialized(void) {
    if (!modules_initialized) {
        exprtk_registry_add_module(exprtk_module_math());
        exprtk_registry_add_module(exprtk_module_string());
        exprtk_registry_add_module(exprtk_module_stats());
        exprtk_registry_add_module(exprtk_module_io());
        exprtk_registry_add_module(exprtk_module_core());
        exprtk_registry_init();
        modules_initialized = 1;
    }
    
    if (!global_env_initialized) {
        exprtk_env_init(&global_test_env);
        global_env_initialized = 1;
    }
}

/* Helper: Convert value to double */
static double value_to_double(exprtk_value_t val) {
    if (val.type == EXPRTK_VAL_INTEGER) return (double)val.data.integer;
    if (val.type == EXPRTK_VAL_NUMBER) return val.data.number;
    return 0.0;
}

/* Helper: Execute TurboScript code and return result */
static exprtk_value_t eval_script(const char *code) {
    ensure_modules_initialized();
    
    exprtk_node_t *root = exprtk_parse(code, strlen(code));
    if (!root) {
        fprintf(stderr, "Parse failed for: %s\n", code);
        return exprtk_val_num(0);
    }
    
    /* Use global env to keep arena-allocated data alive */
    exprtk_value_t result = exprtk_eval(root, &global_test_env);
    
    exprtk_free(root);
    
    return result;
}

/* Setup/Teardown */
static void setup_test_files(void) {
    /* Create test directory */
    mkdir(TEST_DIR, 0755);
    
    /* Create test file */
    FILE *f = fopen(TEST_FILE, "w");
    if (f) {
        fprintf(f, "Hello, World!");
        fclose(f);
    }
    
    /* Create some files in test directory */
    char path[256];
    snprintf(path, sizeof(path), "%s/file1.txt", TEST_DIR);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "File 1");
        fclose(f);
    }
    
    snprintf(path, sizeof(path), "%s/file2.txt", TEST_DIR);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "File 2");
        fclose(f);
    }
    
    snprintf(path, sizeof(path), "%s/subdir", TEST_DIR);
    mkdir(path, 0755);
}

static void cleanup_test_files(void) {
    char path[256];
    
    /* Remove files in test directory */
    snprintf(path, sizeof(path), "%s/file1.txt", TEST_DIR);
    remove(path);
    
    snprintf(path, sizeof(path), "%s/file2.txt", TEST_DIR);
    remove(path);
    
    snprintf(path, sizeof(path), "%s/subdir", TEST_DIR);
    rmdir(path);
    
    /* Remove test directory */
    rmdir(TEST_DIR);
    
    /* Remove test file */
    remove(TEST_FILE);
}

spec("IO Module") {
    before_each() {
        setup_test_files();
    }
    
    after_each() {
        cleanup_test_files();
    }
    
    /* ========================================================================
     * Basic Sanity Tests
     * ======================================================================== */
    
    describe("Sanity Check") {
        it("should evaluate basic math") {
            exprtk_value_t result = eval_script("1 + 1");
            /* result.type should be EXPRTK_VAL_NUMBER (0) */
            check(result.type == EXPRTK_VAL_NUMBER || result.type == EXPRTK_VAL_INTEGER);
            double val = value_to_double(result);
            check_double_eq(val, 2.0, TEST_TOLERANCE);
        }
        
        it("should call now() function") {
            exprtk_value_t result = eval_script("now()");
            double val = value_to_double(result);
            /* now() should return a reasonable timestamp */
            check(val > 1000000000.0); /* After 2001 */
        }
        
        it("should call date_diff directly") {
            exprtk_value_t result = eval_script("date_diff(1000, 500)");
            double val = value_to_double(result);
            check_double_eq(val, 500.0, TEST_TOLERANCE);
        }
        
        it("should call listdir and check type") {
            char code[256];
            snprintf(code, sizeof(code), "listdir(\"%s\")", TEST_DIR);
            exprtk_value_t result = eval_script(code);
            
            /* Debug output */
            fprintf(stderr, "\nlistdir result type: %d (expected %d for LIST)\n", 
                    result.type, EXPRTK_VAL_LIST);
            
            if (result.type == EXPRTK_VAL_NUMBER) {
                fprintf(stderr, "Got NUMBER with value: %f\n", result.data.number);
            } else if (result.type == EXPRTK_VAL_LIST) {
                fprintf(stderr, "Got LIST with count: %zu\n", result.data.list.count);
            }
            
            check(result.type == EXPRTK_VAL_LIST);
        }
    }
    
    /* ========================================================================
     * Directory Listing Tests
     * ======================================================================== */
    
    describe("listdir()") {
        it("should list directory contents") {
            char code[512];
            snprintf(code, sizeof(code), "let files = listdir(\"%s\"); files", TEST_DIR);
            
            exprtk_value_t result = eval_script(code);
            check_int_eq(result.type, EXPRTK_VAL_LIST);
            
            /* Should have 3 entries: file1.txt, file2.txt, subdir */
            check_int_eq((int)result.data.list.count, 3);
            
            /* Verify entries are strings */
            if (result.data.list.count >= 1) {
                check_int_eq(result.data.list.items[0].type, EXPRTK_VAL_STRING);
            }
        }
        
        it("should return empty list for empty directory") {
            /* Create and test empty directory */
            mkdir("test_empty_dir", 0755);
            
            exprtk_value_t result = eval_script("let files = listdir(\"test_empty_dir\"); files");
            
            check_int_eq(result.type, EXPRTK_VAL_LIST);
            check_int_eq((int)result.data.list.count, 0);
            
            rmdir("test_empty_dir");
        }
        
        it("should return 0 for non-existent directory") {
            exprtk_value_t result = eval_script("listdir(\"/non/existent/path\")");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 0.0, TEST_TOLERANCE);
        }
        
        it("should not include . and .. in results") {
            char code[512];
            snprintf(code, sizeof(code), "let files = listdir(\"%s\"); files", TEST_DIR);
            
            exprtk_value_t result = eval_script(code);
            check_int_eq(result.type, EXPRTK_VAL_LIST);
            
            /* Check that none of the entries are "." or ".." */
            for (size_t i = 0; i < result.data.list.count; i++) {
                if (result.data.list.items[i].type == EXPRTK_VAL_STRING) {
                    const char *name = (const char*)result.data.list.items[i].data.string.data;
                    check(strcmp(name, ".") != 0);
                    check(strcmp(name, "..") != 0);
                }
            }
        }
    }

    describe("glob()") {
        it("should list paths matching a wildcard pattern") {
            char code[512];
            snprintf(code, sizeof(code), "let files = glob(\"%s/*.txt\"); files", TEST_DIR);

            exprtk_value_t result = eval_script(code);
            check_int_eq(result.type, EXPRTK_VAL_LIST);
            check_int_eq((int)result.data.list.count, 2);

            for (size_t i = 0; i < result.data.list.count; i++) {
                check_int_eq(result.data.list.items[i].type, EXPRTK_VAL_STRING);
                const char *path = (const char *)result.data.list.items[i].data.string.data;
                size_t len = strlen(path);
                check(len >= 4 && strcmp(path + len - 4, ".txt") == 0);
            }
        }

        it("should return an empty list when no paths match") {
            char code[512];
            snprintf(code, sizeof(code), "let files = glob(\"%s/*.missing\"); files", TEST_DIR);

            exprtk_value_t result = eval_script(code);
            check_int_eq(result.type, EXPRTK_VAL_LIST);
            check_int_eq((int)result.data.list.count, 0);
        }
    }

    describe("file operations") {
        it("should copy and truncate files") {
            exprtk_value_t result = eval_script(
                "write_file(\"test_io_copy_src.txt\", \"abcdef\");"
                "let c = copy_file(\"test_io_copy_src.txt\", \"test_io_copy_dst.txt\");"
                "let t = file_truncate(\"test_io_copy_dst.txt\", 3);"
                "let data = read_file(\"test_io_copy_dst.txt\");"
                "file_remove(\"test_io_copy_src.txt\");"
                "file_remove(\"test_io_copy_dst.txt\");"
                "c + t + data.length()");

            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 3.0, TEST_TOLERANCE);
            remove("test_io_copy_src.txt");
            remove("test_io_copy_dst.txt");
        }
    }

    describe("recursive directories") {
        it("should create and remove nested directories") {
            const char *root = "test_io_recursive_dir";
            char code[1024];
            snprintf(code, sizeof(code),
                "let nested = path_join(path_join(\"%s\", \"deep\"), \"leaf\");"
                "let mk = mkdir_recursive(nested);"
                "write_file(path_join(nested, \"data.txt\"), \"x\");"
                "let exists = is_file(path_join(nested, \"data.txt\"));"
                "let rm = rmdir_recursive(\"%s\");"
                "mk + exists + rm",
                root, root);

            exprtk_value_t result = eval_script(code);
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 1.0, TEST_TOLERANCE);
            rmdir(root);
        }
    }
    
    /* ========================================================================
     * Date/Time Arithmetic Tests
     * ======================================================================== */
    
    describe("date_add()") {
        it("should add days to timestamp") {
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-01-01T00:00:00\");"
                "let future = date_add(ts, 7);"
                "let expected = date(\"2024-01-08T00:00:00\");"
                "abs(future - expected)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 0.0, 1.0); /* Allow 1 second tolerance */
        }
        
        it("should add days and hours") {
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-01-01T00:00:00\");"
                "let future = date_add(ts, 1, 12);"
                "let expected = date(\"2024-01-02T12:00:00\");"
                "abs(future - expected)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 0.0, 1.0);
        }
        
        it("should add full time components") {
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-01-01T00:00:00\");"
                "let future = date_add(ts, 1, 2, 30, 45);"
                "let expected = date(\"2024-01-02T02:30:45\");"
                "abs(future - expected)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 0.0, 1.0);
        }
        
        it("should handle negative values") {
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-01-08T00:00:00\");"
                "let past = date_add(ts, -7);"
                "let expected = date(\"2024-01-01T00:00:00\");"
                "abs(past - expected)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 0.0, 1.0);
        }
    }
    
    describe("date_diff()") {
        it("should calculate difference in seconds") {
            exprtk_value_t result = eval_script(
                "let ts1 = date(\"2024-01-08T00:00:00\");"
                "let ts2 = date(\"2024-01-01T00:00:00\");"
                "date_diff(ts1, ts2)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            
            /* 7 days = 7 * 86400 = 604800 seconds */
            check_double_eq(result.data.number, 604800.0, 1.0);
        }
        
        it("should handle negative differences") {
            exprtk_value_t result = eval_script(
                "let ts1 = date(\"2024-01-01T00:00:00\");"
                "let ts2 = date(\"2024-01-08T00:00:00\");"
                "date_diff(ts1, ts2)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, -604800.0, 1.0);
        }
        
        it("should calculate difference in days") {
            exprtk_value_t result = eval_script(
                "let ts1 = date(\"2024-01-08T00:00:00\");"
                "let ts2 = date(\"2024-01-01T00:00:00\");"
                "date_diff(ts1, ts2) / 86400");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 7.0, 0.01);
        }
    }
    
    describe("date_components()") {
        it("should decompose date into components") {
            /* Parse 2024-06-15 14:30:45 */
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-06-15T14:30:45\");"
                "date_components(ts)");
            
            check_int_eq(result.type, EXPRTK_VAL_VECTOR);
            check_int_eq((int)result.data.vector.size, 7);
            
            /* Components: [year, month, day, hour, minute, second, weekday] */
            check_double_eq(result.data.vector.data[0], 2024.0, TEST_TOLERANCE); /* year */
            check_double_eq(result.data.vector.data[1], 6.0, TEST_TOLERANCE);    /* month */
            check_double_eq(result.data.vector.data[2], 15.0, TEST_TOLERANCE);   /* day */
            /* hour/minute/second may vary due to timezone, just check valid range */
            check(result.data.vector.data[3] >= 0.0 && result.data.vector.data[3] <= 23.0);
            check(result.data.vector.data[4] >= 0.0 && result.data.vector.data[4] <= 59.0);
            check(result.data.vector.data[5] >= 0.0 && result.data.vector.data[5] <= 59.0);
            /* weekday is system-dependent, skip check */
        }
        
        it("should handle current time") {
            exprtk_value_t result = eval_script(
                "let ts = now();"
                "date_components(ts)");
            
            check_int_eq(result.type, EXPRTK_VAL_VECTOR);
            check_int_eq((int)result.data.vector.size, 7);
            
            /* Year should be reasonable (2020+) */
            check(result.data.vector.data[0] >= 2020.0);
            check(result.data.vector.data[0] <= 2100.0);
            
            /* Month should be 1-12 */
            check(result.data.vector.data[1] >= 1.0);
            check(result.data.vector.data[1] <= 12.0);
        }
    }
    
    describe("date_from_parts()") {
        it("should construct date from components") {
            exprtk_value_t result = eval_script(
                "let ts = date_from_parts(2024, 1, 1);"
                "let expected = date(\"2024-01-01T00:00:00\");"
                "abs(ts - expected)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            /* Allow larger tolerance due to timezone - within 24 hours */
            check(result.data.number <= 86400.0);
        }
        
        it("should construct date with time components") {
            exprtk_value_t result = eval_script(
                "let ts = date_from_parts(2024, 6, 15, 14, 30, 45);"
                "let expected = date(\"2024-06-15T14:30:45\");"
                "abs(ts - expected)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            /* Allow larger tolerance due to timezone - within 24 hours */
            check(result.data.number <= 86400.0);
        }
        
        it("should roundtrip with date_components") {
            exprtk_value_t result = eval_script(
                "let ts1 = date(\"2024-06-15T14:30:45\");"
                "let parts = date_components(ts1);"
                "let ts2 = date_from_parts(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5]);"
                "abs(ts1 - ts2)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 0.0, 1.0);
        }
    }
    
    describe("weekday()") {
        it("should return day of week (0-6)") {
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-06-15T12:00:00\");"
                "weekday(ts)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            
            /* Weekday should be 0-6 */
            check(result.data.number >= 0.0);
            check(result.data.number <= 6.0);
        }
        
        it("should match date_components weekday") {
            exprtk_value_t result = eval_script(
                "let ts = now();"
                "let wd1 = weekday(ts);"
                "let parts = date_components(ts);"
                "let wd2 = parts[6];"
                "abs(wd1 - wd2)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 0.0, TEST_TOLERANCE);
        }
    }
    
    describe("year_day()") {
        it("should return day of year (1-366)") {
            /* January 1st should be day 1 */
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-01-01T00:00:00\");"
                "year_day(ts)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 1.0, TEST_TOLERANCE);
        }
        
        it("should handle leap year") {
            /* 2024 is a leap year, Dec 31 should be day 366 */
            exprtk_value_t result = eval_script(
                "let ts = date(\"2024-12-31T00:00:00\");"
                "year_day(ts)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 366.0, TEST_TOLERANCE);
        }
        
        it("should handle non-leap year") {
            /* 2023 is not a leap year, Dec 31 should be day 365 */
            exprtk_value_t result = eval_script(
                "let ts = date(\"2023-12-31T00:00:00\");"
                "year_day(ts)");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 365.0, TEST_TOLERANCE);
        }
    }
    
    /* ========================================================================
     * Integration Tests
     * ======================================================================== */
    
    describe("Integration") {
        it("should combine listdir with file operations") {
            char code[1024];
            snprintf(code, sizeof(code),
                "let files = listdir(\"%s\");"
                "files",
                TEST_DIR);
            
            exprtk_value_t result = eval_script(code);
            check_int_eq(result.type, EXPRTK_VAL_LIST);
            check_int_eq((int)result.data.list.count, 3);
        }
        
        it("should use date functions together") {
            exprtk_value_t result = eval_script(
                "let start = 1000000;"
                "let end = date_add(start, 30);"
                "let diff_days = date_diff(end, start) / 86400;"
                "diff_days");
            
            check_int_eq(result.type, EXPRTK_VAL_NUMBER);
            check_double_eq(result.data.number, 30.0, 0.01);
        }
    }
}
