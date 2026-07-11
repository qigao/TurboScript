#include "re.h"
#include "tinytest.h"

static void check_match_at(const char *pattern, const char *text, int expected_idx,
                           int expected_len) {
  int len = -1;
  int idx = re_match(pattern, text, &len);
  check_int_eq(idx, expected_idx);
  check_int_eq(len, expected_len);
}

static void check_no_match(const char *pattern, const char *text) {
  int len = -1;
  int idx = re_match(pattern, text, &len);
  check_int_eq(idx, -1);
}

suite("tiny regex") {
  group("inverted character classes") {
    it("matches characters outside the listed set") {
      check_match_at("^[^abc]+$", "xyz", 0, 3);
      check_no_match("^[^abc]+$", "xay");
    }

    it("inverts ranges") {
      check_match_at("^[^0-9]+$", "abc", 0, 3);
      check_no_match("^[^0-9]+$", "a7c");
    }

    it("inverts escaped metaclasses") {
      check_match_at("^[^\\d]+$", "abc", 0, 3);
      check_no_match("^[^\\d]+$", "abc7");
    }
  }

  group("branches") {
    it("matches either top-level branch") {
      check_match_at("foo|bar", "xxbarzz", 2, 3);
      check_match_at("foo|bar", "foo", 0, 3);
    }

    it("keeps branch scope inside groups") {
      check_match_at("^(cat|dog)$", "dog", 0, 3);
      check_no_match("^(cat|dog)$", "cow");
    }
  }

  group("group quantifiers") {
    it("repeats a grouped branch") {
      check_match_at("^(a|b)+$", "abba", 0, 4);
      check_no_match("^(a|b)+$", "abbc");
    }

    it("backtracks grouped repeats before matching the suffix") {
      check_match_at("^(a|b)+c$", "abbc", 0, 4);
    }
  }

  group("compile validation") {
    it("rejects malformed groups and quantifiers") {
      check_null(re_compile("(a|b"));
      check_null(re_compile("*a"));
      check_null(re_compile("a**"));
    }
  }
}
