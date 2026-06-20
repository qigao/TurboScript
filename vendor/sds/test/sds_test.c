#include "tinytest.h"
#include "sds.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define STRICMP _stricmp
#else
  #define STRICMP strcasecmp
#endif

#define SDS_NEW_LITERAL(dst, str)                                                                   \
    do {                                                                                            \
        sds __s = sdsnew((str));                                                                    \
        check_not_null(__s);                                                                        \
        check_str_eq(__s, (str));                                                                   \
        check_size_eq(sdslen(__s), strlen((str)));                                                  \
        (dst) = __s;                                                                                \
    } while (0)

#define SDS_NEW_FILLED(dst, ch, nbytes)                                                             \
    do {                                                                                            \
        size_t __n = (nbytes);                                                                      \
        char *__buf = (char *)malloc(__n);                                                          \
        check_not_null(__buf);                                                                      \
        memset(__buf, (ch), __n);                                                                   \
        sds __s = sdsnewlen(__buf, __n);                                                            \
        free(__buf);                                                                                \
        check_not_null(__s);                                                                        \
        check_size_eq(sdslen(__s), __n);                                                            \
        (dst) = __s;                                                                                \
    } while (0)

static void sds_free_all(sds a, sds b, sds c, sds d) {
    if (a) sdsfree(a);
    if (b) sdsfree(b);
    if (c) sdsfree(c);
    if (d) sdsfree(d);
}

spec("SDS library tests") {
    describe("Basics") {
        it("creates and frees strings") {
            sds s;
            SDS_NEW_LITERAL(s, "Hello World");
            sdsfree(s);
        }

        it("handles empty strings") {
            sds s = sdsempty();
            check_not_null(s);
            check_size_eq(sdslen(s), 0);
            check_str_eq(s, "");
            sdsfree(s);
        }
    }

    describe("Concatenation") {
        it("concatenates C strings") {
            sds s;
            SDS_NEW_LITERAL(s, "Hello");
            s = sdscat(s, " World");
            check_str_eq(s, "Hello World");
            check_size_eq(sdslen(s), (size_t)11);
            sdsfree(s);
        }

        it("concatenates sds strings") {
            sds s1;
            sds s2;
            SDS_NEW_LITERAL(s1, "Part 1");
            SDS_NEW_LITERAL(s2, " & Part 2");
            s1 = sdscatsds(s1, s2);
            check_str_eq(s1, "Part 1 & Part 2");
            sds_free_all(s1, s2, NULL, NULL);
        }
    }

    describe("Comparison") {
        it("matches string.h sign behavior") {
            sds s1;
            sds s2;
            sds s3;
            sds s4;
            SDS_NEW_LITERAL(s1, "abc");
            SDS_NEW_LITERAL(s2, "abc");
            SDS_NEW_LITERAL(s3, "abd");
            SDS_NEW_LITERAL(s4, "ab");

            check_int_eq(sdscmp(s1, s2), 0);
            check_int_eq(sdscmp(s1, s2), strcmp("abc", "abc"));

            {
                int res = sdscmp(s1, s3);
                int sys = strcmp("abc", "abd");
                check((res < 0 && sys < 0) || (res > 0 && sys > 0) || (res == 0 && sys == 0));
            }

            {
                int res = sdscmp(s1, s4);
                int sys = strcmp("abc", "ab");
                check((res < 0 && sys < 0) || (res > 0 && sys > 0) || (res == 0 && sys == 0));
            }

            sds_free_all(s1, s2, s3, s4);
        }

        it("case-insensitive comparison") {
            check_int_eq(sdscasecmp("Hello", "hello"), 0);
            check_int_eq(sdscasecmp("ABC", "abc"), 0);
            check_int_eq(sdscasecmp("abc", "ABC"), 0);
            check(sdscasecmp("abc", "abd") < 0);
            check(sdscasecmp("abd", "abc") > 0);
            check_int_eq(sdscasecmp(NULL, NULL), 0);
            check(sdscasecmp(NULL, "a") < 0);
            check(sdscasecmp("a", NULL) > 0);
        }

        it("case-insensitive comparison with length") {
            check_int_eq(sdsncasecmp("Hello", "hello", 5), 0);
            check_int_eq(sdsncasecmp("HelloWorld", "HelloPlanet", 5), 0);
            check(sdsncasecmp("HelloWorld", "HelloPlanet", 6) != 0);
            check_int_eq(sdsncasecmp("abc", "abd", 0), 0);
            check_int_eq(sdsncasecmp(NULL, NULL, 5), 0);
        }

        it("starts with prefix") {
            check(sdsstartswith("Hello World", "Hello"));
            check(sdsstartswith("Hello", "Hello"));
            check(!sdsstartswith("Hello", "hello"));
            check(!sdsstartswith("Hi", "Hello"));
            check(sdsstartswith("abc", ""));
            check(!sdsstartswith(NULL, "a"));
            check(!sdsstartswith("a", NULL));
        }

        it("starts with prefix (case-insensitive)") {
            check(sdsistartswith("Hello World", "hello"));
            check(sdsistartswith("HELLO", "hello"));
            check(sdsistartswith("hello", "HELLO"));
            check(!sdsistartswith("Hi", "Hello"));
        }

        it("ends with suffix") {
            check(sdsendswith("Hello World", "World"));
            check(sdsendswith("Hello", "Hello"));
            check(!sdsendswith("Hello", "hello"));
            check(!sdsendswith("Hi", "Hello"));
            check(sdsendswith("abc", ""));
            check(!sdsendswith(NULL, "a"));
            check(!sdsendswith("a", NULL));
        }

        it("contains substring") {
            check(sdscontains("Hello World", "World"));
            check(sdscontains("Hello World", "o W"));
            check(sdscontains("Hello", "Hello"));
            check(!sdscontains("Hello", "world"));
            check(sdscontains("abc", ""));
            check(!sdscontains(NULL, "a"));
            check(!sdscontains("a", NULL));
        }
    }

    describe("Copying") {
        it("copies strings") {
            sds s;
            SDS_NEW_LITERAL(s, "initial");
            s = sdscpy(s, "new content");
            check_str_eq(s, "new content");
            check_size_eq(sdslen(s), (size_t)11);
            sdsfree(s);
        }
    }

    describe("Trimming") {
        it("trims characters") {
            sds s;
            SDS_NEW_LITERAL(s, "  hello  ");
            s = sdstrim(s, " ");
            check_str_eq(s, "hello");
            check_size_eq(sdslen(s), (size_t)5);
            sdsfree(s);
        }
    }

    describe("Header boundaries") {
        it("uses type5 for length 31 and type8+ for length 32") {
            sds s31;
            sds s32;
            SDS_NEW_FILLED(s31, 'a', 31);
            SDS_NEW_FILLED(s32, 'b', 32);

            check_int_eq((s31[-1] & SDS_TYPE_MASK), SDS_TYPE_5);
            check((s32[-1] & SDS_TYPE_MASK) != SDS_TYPE_5);

            sds_free_all(s31, s32, NULL, NULL);
        }

        it("allows type5 length increments up to 31") {
            sds s;
            SDS_NEW_FILLED(s, 'c', 30);
            check_int_eq((s[-1] & SDS_TYPE_MASK), SDS_TYPE_5);
            sdsinclen(s, 1);
            check_size_eq(sdslen(s), (size_t)31);
            check_int_eq((s[-1] & SDS_TYPE_MASK), SDS_TYPE_5);
            sdsfree(s);
        }
    }

}