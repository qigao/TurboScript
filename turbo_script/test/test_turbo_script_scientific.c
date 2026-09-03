#include "../src/turbo_script_internal.h"
#include "exprtk_types.h"
#include "tinytest.h"
#include "salts_fs.h"
#include "turbo_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  turbo_script_ctx_t *ctx;
  const char *script;
  int result;
} script_coro_arg_t;

static unsigned long ts_test_nonce(void) {
  static unsigned long counter = 0;
  return ((unsigned long)time(NULL) << 16) ^ (unsigned long)clock() ^ ++counter;
}

static void ts_test_make_name(char *buf, size_t buf_size, const char *prefix, const char *suffix) {
  const char *tail = suffix ? suffix : "";
  if (!buf || buf_size == 0 || !prefix) return;
  snprintf(buf, buf_size, "%s_%lu%s", prefix, ts_test_nonce(), tail);
}

static exprtk_value_t test_triple_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  (void)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0};
  }
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = args[0].data.number * 3.0};
}


spec("turbo_script_scientific") {
  describe("Scientific: Advanced Statistics") {
    it("should compute median, percentile, skewness, kurtosis") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [1, 2, 3, 4, "
                           "5, 6, 7, 8, 9, 10]; "
                           "med = median(v); "
                           "p75 = percentile(v, 75); "
                           "sk = skewness(v); "
                           "ku = kurtosis(v); "
                           "gm = geometric_mean(v); "
                           "hm = harmonic_mean(v);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Stats Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      double med = ts_get_num(ctx, "med");
      double p75 = ts_get_num(ctx, "p75");
      double sk = ts_get_num(ctx, "sk");
      double gm = ts_get_num(ctx, "gm");
      double hm = ts_get_num(ctx, "hm");
      printf("  Median: %.2f\n", med);
      printf("  P75: %.2f\n", p75);
      printf("  Skewness: %.4f\n", sk);
      printf("  Geometric Mean: %.4f\n", gm);
      printf("  Harmonic Mean: %.4f\n", hm);

      check(fabs((double)(med) - (double)(5.5)) <= (double)(0.1));
      check((p75) > (med));
      // Uniform distribution: skewness ≈ 0
      check(fabs((double)(sk) - (double)(0.0)) <= (double)(0.5));
      // GM < AM < nothing, HM < GM
      check((hm) < (gm));

      turbo_script_free(ctx);
    }

    it("should compute cumsum, cumprod, rank, zscore") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [3, 1, 4, 1, 5]; "
                           "cs = vec.cumsum(v); "
                           "rk = rank(v); "
                           "zs = zscore(v); "
                           "cs_last = cs[4]; "
                           "cs_len = vec.len(cs);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Stats2 Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "cs_last")) - (double)(14.0)) <= (double)(0.001)); // 3+1+4+1+5
      check(fabs((double)(ts_get_num(ctx, "cs_len")) - (double)(5.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Time Series") {
    it("should compute diff, autocorrelation, hurst exponent") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ts");
      turbo_script_load_plugin(ctx, "vec");
      const char *script = "v = [100, 102, "
                           "101, 105, 103, 107, 106, 110, 108, 112, "
                           "     111, 115, 113, 117, 116, 120, 118, 122, 121, 125]; "
                           "d = ts.diff(v, 1); "
                           "d_len = vec.len(d); "
                           "ac = ts.autocorr(v, 5); "
                           "ac_len = vec.len(ac); "
                           "h = ts.hurst(v);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("TimeSeries Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      double d_len = ts_get_num(ctx, "d_len");
      double ac_len = ts_get_num(ctx, "ac_len");
      printf("  Diff len: %.0f\n", d_len);
      printf("  Autocorr len: %.0f\n", ac_len);
      check((d_len) > (0.0));
      check((ac_len) > (0.0));

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Calculus") {
    it("should integrate and differentiate script functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          // Define f(x) = x^2, integrate from 0 to 3 → should be 9
          "func f(x) { return x * x; }; "
          "area = integrate(\"f\", 0, 3, 1000); "
          // derivative of x^2 at x=2 → should be 4
          "slope = derivative(\"f\", 2);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Calculus Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      double area = ts_get_num(ctx, "area");
      double slope = ts_get_num(ctx, "slope");
      printf("  Integral of x^2 from 0..3: %.4f (expected 9)\n", area);
      printf("  Derivative of x^2 at x=2: %.4f (expected 4)\n", slope);

      check(fabs((double)(area) - (double)(9.0)) <= (double)(0.01));
      check(fabs((double)(slope) - (double)(4.0)) <= (double)(0.01));

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Math Builtins") {
    it("should support trig, log, exp, rounding") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "s = sin(0); "
                           "c = cos(0); "
                           "sq = sqrt(144); "
                           "lg = log(e); "
                           "ex = exp(1); "
                           "cl = ceil(2.3); "
                           "fl = floor(2.7); "
                           "rn = round(2.5); "
                           "ab = abs(-42); "
                           "fib = fibonacci(10); "
                           "g = gcd(12, 8);";
      check((turbo_script_run(ctx, script)) == (0));

      check(fabs((double)(ts_get_num(ctx, "s")) - (double)(0.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "sq")) - (double)(12.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "lg")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "cl")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "fl")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rn")) - (double)(3.0)) <= (double)(1.0)); // platform-dependent rounding
      check(fabs((double)(ts_get_num(ctx, "ab")) - (double)(42.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "fib")) - (double)(55.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "g")) - (double)(4.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: String Builtins") {
    it("should support lower, upper, trim, contains, substr, replace") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "lo = lower(\"HELLO\"); "
                           "up = upper(\"hello\"); "
                           "tr = trim(\"  hi  \"); "
                           "ct = contains(\"foobar\", \"bar\"); "
                           "sw = starts_with(\"foobar\", \"foo\"); "
                           "ew = ends_with(\"foobar\", \"bar\"); "
                           "ix = index_of(\"foobar\", \"bar\"); "
                           "ss = substr(\"hello world\", 6, 5); "
                           "rp = replace(\"aabbcc\", \"bb\", \"XX\");";
      check((turbo_script_run(ctx, script)) == (0));

      check(fabs((double)(ts_get_num(ctx, "ct")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "sw")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ew")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ix")) - (double)(3.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }

    it("should tokenize and convert strings through string views") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "data = \"10,,20;30\"; "
                           "count = token_count(data, \",;\"); "
                           "tok = str_token(data, \",;\", 1); "
                           "vals = split(data, \",;\"); "
                           "total = vals[0] + vals[1] + vals[2]; "
                           "num = to_num(\"42.5\"); "
                           "ival = to_int(\"0x10\"); "
                           "truth = to_bool(\"YES\");";
      check((turbo_script_run(ctx, script)) == (0));

      check(fabs((double)(ts_get_num(ctx, "count")) - (double)(3.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "tok")), ("20")) == 0);
      check(fabs((double)(ts_get_num(ctx, "total")) - (double)(60.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "num")) - (double)(42.5)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ival")) - (double)(16.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "truth")) - (double)(1.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }

    it("should support utf8 codepoint string operations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "b\"; "
                           "var valid = utf8_valid(text); "
                           "var len = utf8_len(text); "
                           "var idx = utf8_index_of(text, \"" "\xF0\x9F\x99\x82" "\"); "
                           "var slice = utf8_substr(text, 1, 2); "
                           "var upper = utf8_upper(\"" "\xC3\xA9" "\"); "
                           "var lower = utf8_lower(\"" "\xC3\x89" "\"); "
                           "var total = valid + len + idx;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("UTF8 string Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "total")) - (double)(7.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "slice")), ("\xE4\xBD\xA0" "\xF0\x9F\x99\x82")) == 0);
      check(strcmp((ts_get_str(ctx, "upper")), ("\xC3\x89")) == 0);
      check(strcmp((ts_get_str(ctx, "lower")), ("\xC3\xA9")) == 0);

      turbo_script_free(ctx);
    }

    it("should split strings by substring and join string lists") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var parts = str_split(\"alpha--beta--\", \"--\"); "
                           "var joined = str_join(parts, \"|\"); "
                           "var chars = str_split(\"a" "\xE4\xBD\xA0" "b\", \"\"); "
                           "var score = parts.length() + parts[0].length() + "
                           "parts[1].length() + parts[2].length() + chars.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String split/join Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(15.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "joined")), ("alpha|beta|")) == 0);

      turbo_script_free(ctx);
    }

    it("should expose utf8 character helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "\xE4\xBD\xA0" "\"; "
                           "var ch = utf8_char_at(text, 1); "
                           "var cp = utf8_codepoint_at(text, 1); "
                           "var last = utf8_rindex_of(text, \"" "\xE4\xBD\xA0" "\"); "
                           "var rev = utf8_reverse(text); "
                           "var score = cp + last + utf8_len(rev);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("UTF8 helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(20327.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "ch")), ("\xE4\xBD\xA0")) == 0);
      check(strcmp((ts_get_str(ctx, "rev")), ("\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "\xE4\xBD\xA0" "a")) == 0);

      turbo_script_free(ctx);
    }

    it("should expose codepoint and byte helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "b\"; "
                           "var cp0 = ord(\"" "\xE4\xBD\xA0" "\"); "
                           "var cp1 = codepoint_at(text, 2); "
                           "var cp2 = text.codePointAt(1); "
                           "var built = from_codepoint(20320, 128578); "
                           "var single = chr(20320); "
                           "var sliced = utf8_slice(text, -3, -1); "
                           "var byte = byte_at(\"Az\", 1); "
                           "var bytes = byte_length(\"" "\xE4\xBD\xA0" "\"); "
                           "var score = cp0 + cp1 + cp2 + byte + bytes;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Codepoint helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(169343.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "built")), ("\xE4\xBD\xA0" "\xF0\x9F\x99\x82")) == 0);
      check(strcmp((ts_get_str(ctx, "single")), ("\xE4\xBD\xA0")) == 0);
      check(strcmp((ts_get_str(ctx, "sliced")), ("\xE4\xBD\xA0" "\xF0\x9F\x99\x82")) == 0);

      turbo_script_free(ctx);
    }

    it("should support data-oriented string helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var escaped = csv_escape(\"a,\\\"b\\\"\\n\"); "
                           "var raw = csv_unescape(escaped); "
                           "var lines = str_lines(\"a\\r\\nb\\nc\\r\"); "
                           "var repeated = str_repeat(\"ab\", 3); "
                           "var count = str_count_substr(\"aaaa\", \"aa\"); "
                           "var score = lines.length() + count;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Data string helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(5.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "escaped")), ("\"a,\"\"b\"\"\n\"")) == 0);
      check(strcmp((ts_get_str(ctx, "raw")), ("a,\"b\"\n")) == 0);
      check(strcmp((ts_get_str(ctx, "repeated")), ("ababab")) == 0);

      turbo_script_free(ctx);
    }

    it("should support trimming padding and case-insensitive helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var trimmed = trim_chars(\"--value--\", \"-\"); "
                           "var left = ltrim_chars(\"..value..\", \".\"); "
                           "var right = rtrim_chars(\"..value..\", \".\"); "
                           "var ci_eq = str_eq_ci(\"" "\xC3\x89" "\", \"" "\xC3\xA9" "\"); "
                           "var ci_contains = str_contains_ci(\"a" "\xC3\x89" "b\", \"" "\xC3\xA9" "\"); "
                           "var once = replace_once(\"aaaa\", \"aa\", \"b\"); "
                           "var no_prefix = remove_prefix(\"pre-name\", \"pre-\"); "
                           "var no_suffix = remove_suffix(\"name.txt\", \".txt\"); "
                           "var padded_left = lpad(\"" "\xE4\xBD\xA0" "\", 3, \"" "\xF0\x9F\x99\x82" "\"); "
                           "var padded_right = rpad(\"x\", 3, \".\"); "
                           "var bad_prefix = starts_with_ci(\"" "\xE4\xBD\xA0" "a\", \"n\"); "
                           "var bad_suffix = ends_with_ci(\"a" "\xE4\xBD\xA0" "\", \"x\"); "
                           "var score = ci_eq + ci_contains;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String cleanup helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(2.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "trimmed")), ("value")) == 0);
      check(strcmp((ts_get_str(ctx, "left")), ("value..")) == 0);
      check(strcmp((ts_get_str(ctx, "right")), ("..value")) == 0);
      check(strcmp((ts_get_str(ctx, "once")), ("baa")) == 0);
      check(strcmp((ts_get_str(ctx, "no_prefix")), ("name")) == 0);
      check(strcmp((ts_get_str(ctx, "no_suffix")), ("name")) == 0);
      check(strcmp((ts_get_str(ctx, "padded_left")), ("\xF0\x9F\x99\x82" "\xF0\x9F\x99\x82" "\xE4\xBD\xA0")) == 0);
      check(strcmp((ts_get_str(ctx, "padded_right")), ("x..")) == 0);
      check(fabs((double)(ts_get_num(ctx, "bad_prefix")) - (double)(0.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "bad_suffix")) - (double)(0.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }

    it("should support partition word and csv line helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var sw = starts_with_ci(\"Hello\", \"he\"); "
                           "var ew = ends_with_ci(\"Hello\", \"LO\"); "
                           "var p = str_partition(\"a=b=c\", \"=\"); "
                           "var rp = str_rpartition(\"a=b=c\", \"=\"); "
                           "var words = str_words(\"  alpha\\t beta\\n gamma  \"); "
                           "var cells = csv_split_line(\"a,\\\"b,c\\\",\\\"d\\\"\\\"e\\\"\"); "
                           "var line = csv_join_line(list(\"a\", \"b,c\", \"d\\\"e\")); "
                           "var p0 = p[0]; "
                           "var p1 = p[1]; "
                           "var p2 = p[2]; "
                           "var rp0 = rp[0]; "
                           "var rp1 = rp[1]; "
                           "var rp2 = rp[2]; "
                           "var word1 = words[1]; "
                           "var cell1 = cells[1]; "
                           "var cell2 = cells[2]; "
                           "var score = sw + ew + p.length() + rp.length() + words.length() + cells.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String partition/csv helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(14.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "p0")), ("a")) == 0);
      check(strcmp((ts_get_str(ctx, "p1")), ("=")) == 0);
      check(strcmp((ts_get_str(ctx, "p2")), ("b=c")) == 0);
      check(strcmp((ts_get_str(ctx, "rp0")), ("a=b")) == 0);
      check(strcmp((ts_get_str(ctx, "rp1")), ("=")) == 0);
      check(strcmp((ts_get_str(ctx, "rp2")), ("c")) == 0);
      check(strcmp((ts_get_str(ctx, "word1")), ("beta")) == 0);
      check(strcmp((ts_get_str(ctx, "cell1")), ("b,c")) == 0);
      check(strcmp((ts_get_str(ctx, "cell2")), ("d\"e")) == 0);
      check(strcmp((ts_get_str(ctx, "line")), ("a,\"b,c\",\"d\"\"e\"")) == 0);

      turbo_script_free(ctx);
    }

    it("should support Python and JS style string helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var fmt = str_format(\"{}:{1}:{{}}\", \"x\", 42); "
                           "var norm = unicode_normalize(\"" "\xC3\xA9" "\", \"NFC\"); "
                           "var gl = grapheme_len(\"a" "\xE4\xBD\xA0" "b\"); "
                           "var gs = grapheme_substr(\"a" "\xE4\xBD\xA0" "b\", 1, 1); "
                           "var gr = grapheme_reverse(\"a" "\xE4\xBD\xA0" "b\"); "
                           "var split = grapheme_split(\"a" "\xE4\xBD\xA0" "b\"); "
                           "var titled = title(\"hello turbo script\"); "
                           "var cap = capitalize(\"hELLO\"); "
                           "var swapped = swapcase(\"AbC1\"); "
                           "var folded = casefold(\"" "\xC3\x89" "\"); "
                           "var removed = remove_chars(\"a" "\xE4\xBD\xA0" "b" "\xE4\xBD\xA0" "\", \"" "\xE4\xBD\xA0" "\"); "
                           "var kept = keep_chars(\"a1b2c3\", \"123\"); "
                           "var trans = translate(\"abcxyz\", \"abc\", \"123\"); "
                           "var score = gl + split.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Python/JS string helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(6.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "fmt")), ("x:42:{}")) == 0);
      check(strcmp((ts_get_str(ctx, "norm")), ("\xC3\xA9")) == 0);
      check(strcmp((ts_get_str(ctx, "gs")), ("\xE4\xBD\xA0")) == 0);
      check(strcmp((ts_get_str(ctx, "gr")), ("b" "\xE4\xBD\xA0" "a")) == 0);
      check(strcmp((ts_get_str(ctx, "titled")), ("Hello Turbo Script")) == 0);
      check(strcmp((ts_get_str(ctx, "cap")), ("Hello")) == 0);
      check(strcmp((ts_get_str(ctx, "swapped")), ("aBc1")) == 0);
      check(strcmp((ts_get_str(ctx, "folded")), ("\xC3\xA9")) == 0);
      check(strcmp((ts_get_str(ctx, "removed")), ("ab")) == 0);
      check(strcmp((ts_get_str(ctx, "kept")), ("123")) == 0);
      check(strcmp((ts_get_str(ctx, "trans")), ("123xyz")) == 0);

      turbo_script_free(ctx);
    }

    it("should support string predicates and Python JS search helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var score = is_empty(\"\") + is_blank(\" \\t\\n\") + "
                           "is_ascii(\"abc\") + is_digit(\"123\") + is_alpha(\"abc\") + "
                           "is_alnum(\"abc123\") + is_space(\" \\n\") + "
                           "is_lower(\"abc1\") + is_upper(\"ABC1\"); "
                           "var last = last_index_of(\"ababa\", \"ba\"); "
                           "var f = find(\"ababa\", \"ba\", 2); "
                           "var rf = rfind(\"ababa\", \"ba\"); "
                           "var sliced = slice(\"abcdef\", -4, -1); "
                           "var ch = char_at(\"abc\", 1); "
                           "var neg = is_digit(\"12a\") + is_ascii(\"" "\xE4\xBD\xA0" "\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String predicate/search Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(9.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "f")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rf")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "neg")) - (double)(0.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "sliced")), ("cde")) == 0);
      check(strcmp((ts_get_str(ctx, "ch")), ("b")) == 0);

      turbo_script_free(ctx);
    }

    it("should support html url base64 and hex string codecs") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var html = html_escape(\"<a&\\\"'>\"); "
                           "var html_raw = html_unescape(html); "
                           "var url = url_encode(\"a b+c/" "\xE4\xBD\xA0" "\"); "
                           "var url_raw = url_decode(url); "
                           "var b64 = base64_encode(\"hello\"); "
                           "var b64_raw = base64_decode(b64); "
                           "var hx = hex_encode(\"Az\"); "
                           "var hx_raw = hex_decode(hx);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String codec Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "html")), ("&lt;a&amp;&quot;&#39;&gt;")) == 0);
      check(strcmp((ts_get_str(ctx, "html_raw")), ("<a&\"'>")) == 0);
      check(strcmp((ts_get_str(ctx, "url")), ("a%20b%2Bc%2F%E4%BD%A0")) == 0);
      check(strcmp((ts_get_str(ctx, "url_raw")), ("a b+c/" "\xE4\xBD\xA0")) == 0);
      check(strcmp((ts_get_str(ctx, "b64")), ("aGVsbG8=")) == 0);
      check(strcmp((ts_get_str(ctx, "b64_raw")), ("hello")) == 0);
      check(strcmp((ts_get_str(ctx, "hx")), ("417a")) == 0);
      check(strcmp((ts_get_str(ctx, "hx_raw")), ("Az")) == 0);

      turbo_script_free(ctx);
    }

    it("should support advanced string search split predicates and codecs") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var hits = find_all(\"a--b--c--\", \"--\"); "
                           "var limited = split_limit(\"a,b,c,d\", \",\", 2); "
                           "var b64u = base64url_encode(\"hello?\"); "
                           "var b64u_raw = base64url_decode(b64u); "
                           "var hex_ok = is_hex(\"0aF9\"); "
                           "var hex_bad = is_hex(\"0x10\"); "
                           "var printable_ok = is_printable(\"line\\n\"); "
                           "var printable_bad = is_printable(chr(1)); "
                           "var eq_ok = constant_time_eq(\"secret\", \"secret\"); "
                           "var eq_bad = constant_time_eq(\"secret\", \"secreT\"); "
                           "var score = hits.length() + hits[0] + hits[1] + hits[2] + "
                           "limited.length() + hex_ok + printable_ok + eq_ok + "
                           "hex_bad + printable_bad + eq_bad; "
                           "var part0 = limited[0]; "
                           "var part1 = limited[1]; "
                           "var part2 = limited[2];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Advanced string helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(21.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "part0")), ("a")) == 0);
      check(strcmp((ts_get_str(ctx, "part1")), ("b")) == 0);
      check(strcmp((ts_get_str(ctx, "part2")), ("c,d")) == 0);
      check(strcmp((ts_get_str(ctx, "b64u")), ("aGVsbG8_")) == 0);
      check(strcmp((ts_get_str(ctx, "b64u_raw")), ("hello?")) == 0);

      turbo_script_free(ctx);
    }

    it("should support overlapping search wrap case conversion and bytes") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var hits = find_all_overlapping(\"aaaa\", \"aa\"); "
                           "var overlap_count = str_count_overlapping(\"aaaa\", \"aa\"); "
                           "var wrapped = word_wrap(\"alpha beta gamma\", 10); "
                           "var hard_wrap = word_wrap(\"abcdefgh\", 3); "
                           "var unbroken = word_wrap(\"abcdefgh\", 3, 0); "
                           "var short = shorten(\"alpha beta gamma\", 14); "
                           "var tiny = shorten(\"abcdef\", 2); "
                           "var snake = snake_case(\"Hello HTTP response_code42\"); "
                           "var kebab = kebab_case(\"Hello HTTP response_code42\"); "
                           "var camel = camel_case(\"Hello HTTP response_code42\"); "
                           "var pascal = pascal_case(\"Hello HTTP response_code42\"); "
                           "var slug = slugify(\"Hello, Turbo Script!\"); "
                           "var bs = bytes(\"Az\"); "
                           "var raw = from_bytes(bs); "
                           "var raw_vec = from_bytes([65,66]); "
                           "var type_score = (typeof(bs) == \"bytes\") + is_bytes(bs); "
                           "var score = hits.length() + hits[0] + hits[1] + hits[2] + "
                           "overlap_count + bs.length() + bs[0] + bs[1] + type_score;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String extended helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(200.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "wrapped")), ("alpha beta\ngamma")) == 0);
      check(strcmp((ts_get_str(ctx, "hard_wrap")), ("abc\ndef\ngh")) == 0);
      check(strcmp((ts_get_str(ctx, "unbroken")), ("abcdefgh")) == 0);
      check(strcmp((ts_get_str(ctx, "short")), ("alpha beta...")) == 0);
      check(strcmp((ts_get_str(ctx, "tiny")), ("..")) == 0);
      check(strcmp((ts_get_str(ctx, "snake")), ("hello_http_response_code42")) == 0);
      check(strcmp((ts_get_str(ctx, "kebab")), ("hello-http-response-code42")) == 0);
      check(strcmp((ts_get_str(ctx, "camel")), ("helloHttpResponseCode42")) == 0);
      check(strcmp((ts_get_str(ctx, "pascal")), ("HelloHttpResponseCode42")) == 0);
      check(strcmp((ts_get_str(ctx, "slug")), ("hello-turbo-script")) == 0);
      check(strcmp((ts_get_str(ctx, "raw")), ("Az")) == 0);
      check(strcmp((ts_get_str(ctx, "raw_vec")), ("AB")) == 0);

      turbo_script_free(ctx);
    }

    it("should support whitespace normalization and split variants") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var normalized = normalize_space(\"  alpha\\t beta\\n  gamma  \"); "
                           "var centered = center(\"x\", 5, \".\"); "
                           "var truncated = truncate(\"abcdef\", 5); "
                           "var short_suffix = truncate(\"abcdef\", 2); "
                           "var stripped = strip_suffix(strip_prefix(\"pre-name.txt\", \"pre-\"), \".txt\"); "
                           "var words = split_whitespace(\"  a\\t b\\n c  \"); "
                           "var once = split_once(\"a=b=c\", \"=\"); "
                           "var ronce = rsplit_once(\"a=b=c\", \"=\"); "
                           "var rparts = rsplit(\"a,b,c\", \",\", 1); "
                           "var score = words.length() + once.length() + ronce.length() + rparts.length(); "
                           "var word1 = words[1]; "
                           "var once0 = once[0]; "
                           "var once1 = once[1]; "
                           "var ronce0 = ronce[0]; "
                           "var ronce1 = ronce[1]; "
                           "var rpart0 = rparts[0]; "
                           "var rpart1 = rparts[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String split variant Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(9.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "normalized")), ("alpha beta gamma")) == 0);
      check(strcmp((ts_get_str(ctx, "centered")), ("..x..")) == 0);
      check(strcmp((ts_get_str(ctx, "truncated")), ("ab...")) == 0);
      check(strcmp((ts_get_str(ctx, "short_suffix")), ("..")) == 0);
      check(strcmp((ts_get_str(ctx, "stripped")), ("name")) == 0);
      check(strcmp((ts_get_str(ctx, "word1")), ("b")) == 0);
      check(strcmp((ts_get_str(ctx, "once0")), ("a")) == 0);
      check(strcmp((ts_get_str(ctx, "once1")), ("b=c")) == 0);
      check(strcmp((ts_get_str(ctx, "ronce0")), ("a=b")) == 0);
      check(strcmp((ts_get_str(ctx, "ronce1")), ("c")) == 0);
      check(strcmp((ts_get_str(ctx, "rpart0")), ("a,b")) == 0);
      check(strcmp((ts_get_str(ctx, "rpart1")), ("c")) == 0);

      turbo_script_free(ctx);
    }

    it("should support line and fixed width string helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "b\"; "
                           "var left_part = left(text, 3); "
                           "var right_part = right(text, 2); "
                           "var taken = str_take(\"abcdef\", 2); "
                           "var dropped = str_drop(\"abcdef\", 2); "
                           "var filled = zfill(\"-42\", 5); "
                           "var expanded = expand_tabs(\"a\\tb\\n\\tc\", 4); "
                           "var chomped = chomp(\"line\\r\\n\"); "
                           "var lines = split_lines(\"a\\r\\nb\\nc\\r\"); "
                           "var line1 = lines[1]; "
                           "var score = lines.length() + line_count(\"a\\r\\nb\\nc\\r\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String line/fixed helper Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(6.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "left_part")), ("a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82")) == 0);
      check(strcmp((ts_get_str(ctx, "right_part")), ("\xF0\x9F\x99\x82" "b")) == 0);
      check(strcmp((ts_get_str(ctx, "taken")), ("ab")) == 0);
      check(strcmp((ts_get_str(ctx, "dropped")), ("cdef")) == 0);
      check(strcmp((ts_get_str(ctx, "filled")), ("-0042")) == 0);
      check(strcmp((ts_get_str(ctx, "expanded")), ("a   b\n    c")) == 0);
      check(strcmp((ts_get_str(ctx, "chomped")), ("line")) == 0);
      check(strcmp((ts_get_str(ctx, "line1")), ("b")) == 0);

      turbo_script_free(ctx);
    }

    it("should support block indentation and wrapping helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var indented = indent(\"a\\nb\", \"> \"); "
                           "var skipped = indent(\"a\\nb\", \"--\", 0); "
                           "var dedented = dedent(\"  a\\n    b\\n\"); "
                           "var surrounded = surround(\"value\", \"[\", \"]\"); "
                           "var unwrapped = unwrap(surrounded, \"[\", \"]\"); "
                           "var unchanged = unwrap(\"value\", \"[\", \"]\"); "
                           "var score = line_count(indented) + line_count(dedented);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String indent/wrap Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(4.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "indented")), ("> a\n> b")) == 0);
      check(strcmp((ts_get_str(ctx, "skipped")), ("a\n--b")) == 0);
      check(strcmp((ts_get_str(ctx, "dedented")), ("a\n  b\n")) == 0);
      check(strcmp((ts_get_str(ctx, "surrounded")), ("[value]")) == 0);
      check(strcmp((ts_get_str(ctx, "unwrapped")), ("value")) == 0);
      check(strcmp((ts_get_str(ctx, "unchanged")), ("value")) == 0);

      turbo_script_free(ctx);
    }

    it("should support string range editing helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var inserted = insert(\"ab\", 1, \"XX\"); "
                           "var deleted = delete_range(\"abcdef\", 2, 3); "
                           "var replaced = replace_range(\"abcdef\", -3, 2, \"XY\"); "
                           "var appended = insert(\"ab\", 20, \"!\"); "
                           "var clipped = delete_range(\"abcdef\", 4, 20);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String range edit Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "inserted")), ("aXXb")) == 0);
      check(strcmp((ts_get_str(ctx, "deleted")), ("abf")) == 0);
      check(strcmp((ts_get_str(ctx, "replaced")), ("abcXYf")) == 0);
      check(strcmp((ts_get_str(ctx, "appended")), ("ab!")) == 0);
      check(strcmp((ts_get_str(ctx, "clipped")), ("abcd")) == 0);

      turbo_script_free(ctx);
    }

    it("should support common string aliases and json escaping") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var inc = includes(\"hello\", \"ell\"); "
                           "var repeated = repeat(\"ab\", 2); "
                           "var counted = count(\"aaaa\", \"aa\"); "
                           "var replaced = replace_all(\"aabb\", \"a\", \"x\"); "
                           "var padded_left = padStart(\"x\", 3, \"0\"); "
                           "var padded_right = padEnd(\"x\", 3, \".\"); "
                           "var stripped = strip(\"  hi\\t\"); "
                           "var left = lstrip(\"  hi\"); "
                           "var right = rstrip(\"hi  \"); "
                           "var trimmed_start = trimStart(\"  js\"); "
                           "var trimmed_end = trimEnd(\"js  \"); "
                           "var json = json_escape(\"a\\n\\\"b\\\\c\"); "
                           "var raw = json_unescape(json); "
                           "var unicode = json_unescape(\"\\\\u4F60\\\\uD83D\\\\uDE42\"); "
                           "var score = inc + counted;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String alias/json Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(3.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "repeated")), ("abab")) == 0);
      check(strcmp((ts_get_str(ctx, "replaced")), ("xxbb")) == 0);
      check(strcmp((ts_get_str(ctx, "padded_left")), ("00x")) == 0);
      check(strcmp((ts_get_str(ctx, "padded_right")), ("x..")) == 0);
      check(strcmp((ts_get_str(ctx, "stripped")), ("hi")) == 0);
      check(strcmp((ts_get_str(ctx, "left")), ("hi")) == 0);
      check(strcmp((ts_get_str(ctx, "right")), ("hi")) == 0);
      check(strcmp((ts_get_str(ctx, "trimmed_start")), ("js")) == 0);
      check(strcmp((ts_get_str(ctx, "trimmed_end")), ("js")) == 0);
      check(strcmp((ts_get_str(ctx, "json")), ("a\\n\\\"b\\\\c")) == 0);
      check(strcmp((ts_get_str(ctx, "raw")), ("a\n\"b\\c")) == 0);
      check(strcmp((ts_get_str(ctx, "unicode")), ("\xE4\xBD\xA0" "\xF0\x9F\x99\x82")) == 0);

      turbo_script_free(ctx);
    }

    it("should render mustache templates from script maps and lists") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "name:\"Ada\","
          "user:map{city:\"Paris\"},"
          "html:\"<b>\","
          "items:list(map{name:\"one\"}, map{name:\"two\"}),"
          "nums:[1,2],"
          "empty:list(),"
          "flag:1"
          "};"
          "var rendered = template_render(\"Hi {{name}} {{user.city}} {{html}} {{{html}}} "
          "{{#items}}{{name}},{{/items}}{{#nums}}{{.}};{{/nums}}{{^empty}}none{{/empty}} "
          "{{#flag}}yes{{/flag}}\", data);"
          "var alias = mustache_render(\"{{name}}\", data);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache template Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "rendered")), ("Hi Ada Paris &lt;b&gt; <b> one,two,1;2;none yes")) == 0);
      check(strcmp((ts_get_str(ctx, "alias")), ("Ada")) == 0);

      turbo_script_free(ctx);
    }

    it("should render mustache lambdas from script functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "name:\"Ada\","
          "emit:func(raw) { return \"{{name}}\"; },"
          "wrap:func(raw) { return \"[{{name}}:\" + raw + \"]\"; }"
          "};"
          "var rendered = template_render(\"{{emit}} {{#wrap}}inside{{/wrap}}\", data);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache lambda Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "rendered")), ("Ada [Ada:inside]")) == 0);

      turbo_script_free(ctx);
    }

    it("should render mustache partials from a script map") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "items:list(map{name:\"one\"}, map{name:\"two\"}),"
          "total:2"
          "};"
          "var partials = map{"
          "item:\"{{name}};\","
          "summary:\"count={{total}}\","
          "outer:\"{{#items}}{{>item}}{{/items}}{{>summary}}\""
          "};"
          "var rendered = template_render(\"{{>outer}}\", data, partials);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache partial Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "rendered")), ("one;two;count=2")) == 0);

      turbo_script_free(ctx);
    }

    it("should render repeated mustache partials and ignore missing partials") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{name:\"Ada\"};"
          "var partials = map{item:\"{{name}}\"};"
          "var rendered = template_render(\"{{>item}}/{{>item}}/{{>missing}}\", data, partials);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache repeated partial Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "rendered")), ("Ada/Ada/")) == 0);

      turbo_script_free(ctx);
    }

    it("should stringify scalar mustache lambda results") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "num:func(raw) { return 42; },"
          "null_value:func(raw) { return null; }"
          "};"
          "var rendered = template_render(\"{{num}}/{{null_value}}\", data);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache scalar lambda Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "rendered")), ("42/null")) == 0);

      turbo_script_free(ctx);
    }

    it("should render nested mustache partial names with parent context lookup") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "total:2,"
          "items:list(map{name:\"one\"}, map{name:\"two\"})"
          "};"
          "var partials = map{layout:map{item:\"{{name}}/{{total}};\"}};"
          "var rendered = template_render(\"{{#items}}{{>layout.item}}{{/items}}\", data, partials);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache nested partial Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(strcmp((ts_get_str(ctx, "rendered")), ("one/2;two/2;")) == 0);

      turbo_script_free(ctx);
    }

  }

  describe("Scientific: Regex Builtins") {
    it("should expose core regex functions without loading a plugin") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var h = regex.compile(\"[0-9]+\"); "
                           "func has_digits(s) { return regex.search(h, s); } "
                           "var full = regex.match(h, \"12345\"); "
                           "var no_full = regex.match(h, \"123abc\"); "
                           "var pos = has_digits(\"abc123def\"); "
                           "var direct = regex.test(\"[a-z]+\", \"hello\"); "
                           "var freed = regex.free(h); "
                           "var score = full + no_full + pos + direct + freed;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Core regex Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(6.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should support regex replace split and find_all") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var replaced = regex.replace(\"[0-9]+\", \"a12b345c\", \"#\"); "
                           "var once = regex.replace(\"[0-9]+\", \"a12b345c\", \"#\", 1); "
                           "var parts = regex.split(\"[|,]+\", \"a,b|c\"); "
                           "var matches = regex.find_all(\"[0-9]+\", \"a12b345c\"); "
                           "var p0 = parts[0]; "
                           "var p1 = parts[1]; "
                           "var p2 = parts[2]; "
                           "var m0 = matches[0]; "
                           "var m1 = matches[1]; "
                           "var score = parts.length() + matches.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Core regex transform Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(5.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "replaced")), ("a#b#c")) == 0);
      check(strcmp((ts_get_str(ctx, "once")), ("a#b345c")) == 0);
      check(strcmp((ts_get_str(ctx, "p0")), ("a")) == 0);
      check(strcmp((ts_get_str(ctx, "p1")), ("b")) == 0);
      check(strcmp((ts_get_str(ctx, "p2")), ("c")) == 0);
      check(strcmp((ts_get_str(ctx, "m0")), ("12")) == 0);
      check(strcmp((ts_get_str(ctx, "m1")), ("345")) == 0);

      turbo_script_free(ctx);
    }

    it("should support regex flags match info and iterators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var ci = regex.match(\"abc\", \"ABC\", \"i\"); "
                           "var h = regex.compile(\"[a-z]+\", \"i\"); "
                           "var info = regex.match_info(h, \"12ABC34\"); "
                           "var iter = regex.find_iter(\"[0-9]+\", \"a12b345\"); "
                           "var info_text = info.text; "
                           "var iter0 = iter[0].text; "
                           "var iter1 = iter[1].text; "
                           "var score = ci + info.start + info.end + iter.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Regex match info Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(10.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "info_text")), ("ABC")) == 0);
      check(strcmp((ts_get_str(ctx, "iter0")), ("12")) == 0);
      check(strcmp((ts_get_str(ctx, "iter1")), ("345")) == 0);

      turbo_script_free(ctx);
    }

    it("should support RegExp object API") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var re = RegExp(\"[0-9]+\"); "
                           "var ci = RegExp(\"abc\", \"i\"); "
                           "var found = re.test(\"a12b\"); "
                           "var full = re.match(\"123\"); "
                           "var no_full = re.match(\"a123\"); "
                           "var pos = re.search(\"a12b\"); "
                           "var info = re.exec(\"a12b\"); "
                           "var none = re.exec(\"abc\"); "
                           "var all = re.find_all(\"a12b345\"); "
                           "var source = re.toString(); "
                           "var score = found + full + no_full + pos + info.start + info.end + "
                           "            all.length() + ci.test(\"ABC\") + (none == null);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("RegExp object Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(11.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "source")), ("[0-9]+")) == 0);
      turbo_script_free(ctx);
    }

    it("should support regex literal syntax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var pattern = /\\d{3}-\\d{4}/; "
                           "var ok = pattern.test(\"555-1234\"); "
                           "var bad = pattern.test(\"55-1234\"); "
                           "var pos = pattern.search(\"call 555-1234\"); "
                           "var ci = /abc/i.test(\"ABC\"); "
                           "var div = 10 / 2; "
                           "var source = pattern.toString(); "
                           "var score = ok + bad + pos + ci + div;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Regex literal Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(12.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "source")), ("\\d{3}-\\d{4}")) == 0);
      turbo_script_free(ctx);
    }

    it("should tear down JIT regex handles safely") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var h = regex.compile(\"[0-9]+\"); "
                           "var matched = regex.search(h, \"abc123def\");";
      int res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("JIT regex teardown Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "matched")) - (double)(3.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should not expose capture groups in core regex") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var info = regex.match_info(\"([0-9]+)\", \"a12b\"); "
                           "var has_groups = info.has(\"groups\"); "
                           "var dot_caught = try { regex.groups(\"([0-9]+)\", \"a12b\"); 0 } catch (e) { 1 }; "
                           "var flat_caught = try { regex_groups(\"([0-9]+)\", \"a12b\"); 0 } catch (e) { 1 }; "
                           "var score = has_groups + dot_caught + flat_caught;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Core regex groups rejection Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "has_groups")) - (double)(0.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

  }

  describe("Scientific: Matrix Operations") {
    it("should compute determinant, inverse, matmul for 2x2") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          // Column-major matrix A = [[1,3],[2,4]], det = 1*4 - 3*2 = -2
          "A = [1, 2, 3, 4]; "
          "d = matrix.det2(A); "
          "Ainv = matrix.inv2(A); "
          "Ainv_len = vec.len(Ainv); "
          // Identity check: A * Ainv should be ~identity
          "I = matrix.matmul(A, Ainv, 2, 2, 2); "
          "i00 = I[0]; "
          "i11 = I[3];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "d")) - (double)(-2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "Ainv_len")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "i00")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "i11")) - (double)(1.0)) <= (double)(0.01));

      turbo_script_free(ctx);
    }

    it("should use miniblas linalg backend for matrix helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "I3 = matrix.eye(3); "
          "A = [1, 2, 3, 4]; "
          "B = [5, 6, 7, 8]; "
          "G = matrix.gemm(A, B, 2, 2, 2); "
          "SPD = [4, 2, 2, 3]; "
          "L = matrix.cholesky(SPD, 2); "
          "X = matrix.solve_tri(L, [2, 1], 2, 1); "
          "UD = matrix.udu([4, 0, 0, 9], 2); "
          "U = UD[0]; "
          "D = UD[1]; "
          "i30 = I3[0]; "
          "g0 = G[0]; "
          "g3 = G[3]; "
          "l0 = L[0]; "
          "l1 = L[1]; "
          "l2 = L[2]; "
          "x0 = X[0]; "
          "x1 = X[1]; "
          "u0 = U[0]; "
          "u3 = U[3]; "
          "d0 = D[0]; "
          "d1 = D[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix backend Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "i30")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "g0")) - (double)(23.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "g3")) - (double)(46.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "l0")) - (double)(2.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "l1")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "l2")) - (double)(0.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "x0")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "x1")) - (double)(0.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "u0")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "u3")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "d0")) - (double)(4.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "d1")) - (double)(9.0)) <= (double)(0.01));
      turbo_script_free(ctx);
    }

    it("should allow explicit row-major matrix layout") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [1, 2, 3, 4]; "
          "B = [5, 6, 7, 8]; "
          "R = matrix.matmul(A, B, 2, 2, 2, 1); "
          "G = matrix.gemm(A, B, 2, 2, 2, 1); "
          "G2 = matrix.gemm(A, B, 2, 2, 2, 2, 0, 1); "
          "SPD = [4, 2, 2, 3]; "
          "L = matrix.cholesky(SPD, 2, 1); "
          "X = matrix.solve_tri(L, [2, 1], 2, 1, 1); "
          "UD = matrix.udu([4, 0, 0, 9], 2, 1); "
          "U = UD[0]; "
          "D = UD[1]; "
          "r0 = R[0]; "
          "r1 = R[1]; "
          "r2 = R[2]; "
          "r3 = R[3]; "
          "g0 = G[0]; "
          "g3 = G[3]; "
          "g20 = G2[0]; "
          "g23 = G2[3]; "
          "l0 = L[0]; "
          "l1 = L[1]; "
          "l2 = L[2]; "
          "x0 = X[0]; "
          "x1 = X[1]; "
          "u0 = U[0]; "
          "u3 = U[3]; "
          "d0 = D[0]; "
          "d1 = D[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Row-major matrix Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "r0")) - (double)(19.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "r1")) - (double)(22.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "r2")) - (double)(43.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "r3")) - (double)(50.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "g0")) - (double)(19.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "g3")) - (double)(50.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "g20")) - (double)(38.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "g23")) - (double)(100.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "l0")) - (double)(2.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "l1")) - (double)(0.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "l2")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "x0")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "x1")) - (double)(0.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "u0")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "u3")) - (double)(1.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "d0")) - (double)(4.0)) <= (double)(0.01));
      check(fabs((double)(ts_get_num(ctx, "d1")) - (double)(9.0)) <= (double)(0.01));
      turbo_script_free(ctx);
    }

    it("should expose matrix metadata and generic solve helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [2, 1, 1, 3]; "
          "shape = matrix.shape(A, 2, 2); "
          "info = matrix.info(A, 2, 2); "
          "order = info.order; "
          "dtype = info.dtype; "
          "Inv = matrix.inv(A, 2); "
          "X = matrix.solve(A, [5, 10], 2, 1); "
          "R = matrix.solve([2, 1, 1, 3], [5, 10], 2, 1, 1); "
          "rows = matrix.rows(A, 2, 2); "
          "cols = matrix.cols(A, 2, 2); "
          "s0 = shape[0]; "
          "s1 = shape[1]; "
          "inv0 = Inv[0]; "
          "inv3 = Inv[3]; "
          "x0 = X[0]; "
          "x1 = X[1]; "
          "r0 = R[0]; "
          "r1 = R[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix metadata/solve Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "s0")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "s1")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rows")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "cols")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "inv0")) - (double)(0.6)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "inv3")) - (double)(0.4)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "x0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "x1")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r1")) - (double)(3.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "order")), ("col")) == 0);
      check(strcmp((ts_get_str(ctx, "dtype")), ("f64")) == 0);
      turbo_script_free(ctx);
    }

    it("should compute generic determinant and matrix norms") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Acol = [1, 0, 5, 2, 1, 6, 3, 4, 0]; "
          "Arow = [1, 2, 3, 0, 1, 4, 5, 6, 0]; "
          "Mcol = [1, -4, -2, 5, 3, -6]; "
          "Mrow = [1, -2, 3, -4, 5, -6]; "
          "d_col = matrix.det(Acol, 3); "
          "d_row = matrix.det(Arow, 3, 1); "
          "d_alias = mat_det(Arow, 3, 1); "
          "nf = matrix.norm([1, 2, 3, 4], 2, 2); "
          "nf_row = matrix.norm(Mrow, 2, 3, 1); "
          "nl1 = matrix.norm(Mcol, 2, 3, \"l1\"); "
          "ninf = matrix.norm(Mrow, 2, 3, \"inf\", 1); "
          "n_alias = mat_norm(Mcol, 2, 3, \"inf\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix determinant/norm Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "d_col")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "d_row")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "d_alias")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "nf")) - (double)(sqrt(30.0))) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "nf_row")) - (double)(sqrt(91.0))) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "nl1")) - (double)(9.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ninf")) - (double)(15.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "n_alias")) - (double)(15.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should extract generic matrix trace and diagonal") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 3, 5, 2, 4, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "t_col = matrix.trace(Mcol, 3, 2); "
          "t_row = matrix.trace(Mrow, 3, 2, 1); "
          "t_alias = mat_trace(Mrow, 3, 2, 1); "
          "d_col = matrix.diag(Mcol, 3, 2); "
          "d_row = matrix.diag(Mrow, 3, 2, 1); "
          "d_alias = mat_diag(Mrow, 3, 2, 1); "
          "d0 = d_col[0]; "
          "d1 = d_col[1]; "
          "r0 = d_row[0]; "
          "r1 = d_row[1]; "
          "a0 = d_alias[0]; "
          "a1 = d_alias[1]; "
          "dn = vec.len(d_col);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix trace/diag Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "t_col")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "t_row")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "t_alias")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "d0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "d1")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r1")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a1")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "dn")) - (double)(2.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should transpose generic matrices with explicit layout") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 4, 2, 5, 3, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "Tcol = matrix.transpose(Mcol, 2, 3); "
          "Trow = matrix.transpose(Mrow, 2, 3, 1); "
          "Talias = mat_transpose(Mrow, 2, 3, 1); "
          "Told = transpose(Mrow, 2, 3); "
          "c0 = Tcol[0]; c1 = Tcol[1]; c2 = Tcol[2]; c3 = Tcol[3]; c4 = Tcol[4]; c5 = Tcol[5]; "
          "r0 = Trow[0]; r1 = Trow[1]; r2 = Trow[2]; r3 = Trow[3]; r4 = Trow[4]; r5 = Trow[5]; "
          "a0 = Talias[0]; a5 = Talias[5]; "
          "o0 = Told[0]; o5 = Told[5];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix transpose Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "c0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c1")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c2")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c3")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c4")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c5")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r1")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r2")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r3")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r4")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r5")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a5")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "o0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "o5")) - (double)(6.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should construct dense matrices") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Z = matrix.zeros(2, 3); "
          "O = matrix.ones(2, 3, 1); "
          "F = matrix.full(2, 3, 7); "
          "A = mat_full(2, 3, 4, 1); "
          "S = matrix.shape(F, 2, 3); "
          "zn = vec.len(Z); "
          "z0 = Z[0]; z5 = Z[5]; "
          "o0 = O[0]; o5 = O[5]; "
          "f0 = F[0]; f5 = F[5]; "
          "a0 = A[0]; a5 = A[5]; "
          "s0 = S[0]; s1 = S[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix constructors Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "zn")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "z0")) - (double)(0.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "z5")) - (double)(0.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "o0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "o5")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "f0")) - (double)(7.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "f5")) - (double)(7.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a0")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a5")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "s0")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "s1")) - (double)(3.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should compute elementwise matrix operations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [1, 2, 3, 4]; "
          "B = [10, 20, 30, 40]; "
          "S = matrix.add(A, B, 2, 2); "
          "D = matrix.sub(B, A, 2, 2, 1); "
          "H = matrix.hadamard(A, B, 2, 2); "
          "M = matrix.mul(A, B, 2, 2); "
          "C = matrix.scale(A, 2, 2, 2); "
          "Alias = mat_hadamard(A, B, 2, 2); "
          "s0 = S[0]; s3 = S[3]; "
          "d1 = D[1]; "
          "h2 = H[2]; "
          "m3 = M[3]; "
          "c3 = C[3]; "
          "a2 = Alias[2];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix elementwise Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "s0")) - (double)(11.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "s3")) - (double)(44.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "d1")) - (double)(18.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "h2")) - (double)(90.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "m3")) - (double)(160.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c3")) - (double)(8.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a2")) - (double)(90.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should reduce matrices globally and by axis") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 4, 2, 5, 3, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "s = matrix.sum(Mcol, 2, 3); "
          "avg = matrix.mean(Mcol, 2, 3); "
          "mn = matrix.min(Mcol, 2, 3); "
          "mx = matrix.max(Mcol, 2, 3); "
          "cs = matrix.sum(Mcol, 2, 3, \"col\"); "
          "rs = matrix.sum(Mrow, 2, 3, 1, 1); "
          "cm = matrix.mean(Mrow, 2, 3, 0, 1); "
          "rmax = matrix.max(Mrow, 2, 3, \"row\", 1); "
          "alias = mat_min(Mrow, 2, 3, 0, 1); "
          "cs0 = cs[0]; cs2 = cs[2]; "
          "rs0 = rs[0]; rs1 = rs[1]; "
          "cm0 = cm[0]; cm2 = cm[2]; "
          "rm0 = rmax[0]; rm1 = rmax[1]; "
          "a0 = alias[0]; a2 = alias[2];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix reduce Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "s")) - (double)(21.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "avg")) - (double)(3.5)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "mn")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "mx")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "cs0")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "cs2")) - (double)(9.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rs0")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rs1")) - (double)(15.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "cm0")) - (double)(2.5)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "cm2")) - (double)(4.5)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rm0")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rm1")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a2")) - (double)(3.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should extract matrix rows and columns with explicit layout") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 4, 2, 5, 3, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "r = matrix.row(Mcol, 2, 3, 1); "
          "c = matrix.col(Mrow, 2, 3, 2, 1); "
          "ra = mat_row(Mrow, 2, 3, 0, 1); "
          "ca = mat_col(Mcol, 2, 3, 1); "
          "r0 = r[0]; r2 = r[2]; "
          "c0 = c[0]; c1 = c[1]; "
          "ra0 = ra[0]; ra2 = ra[2]; "
          "ca0 = ca[0]; ca1 = ca[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix row/col Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "r0")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "r2")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c0")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c1")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ra0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ra2")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ca0")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ca1")) - (double)(5.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should support matrix shape utilities dot products and broadcasting") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "M = [1, 2, 3, 4, 5, 6]; "
          "S = matrix.slice(M, 2, 3, 0, 2, 1, 2); "
          "R = matrix.reshape(M, 2, 3, 3, 2); "
          "F = matrix.flatten(M, 2, 3); "
          "C = matrix.copy(M, 2, 3); "
          "E = matrix.eye_like(M, 2, 3); "
          "I = matrix.identity(2); "
          "O = matrix.outer([1, 2], [10, 20, 30]); "
          "AR = matrix.add_row(M, [10, 20, 30], 2, 3); "
          "MC = matrix.mul_col(M, [2, 3], 2, 3); "
          "dotv = matrix.dot([1, 2, 3], [4, 5, 6]); "
          "result = S[0] + S[3] + R[5] + F[0] + C[5] + E[0] + E[3] + I[3] + "
          "O[0] + O[5] + AR[0] + AR[4] + MC[1] + dotv;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix utility Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(179.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should compute matrix variance std argmin and argmax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "M = [1, 4, 2, 5, 3, 6]; "
          "v = matrix.variance(M, 2, 3); "
          "s = matrix.std(M, 2, 3); "
          "amin = matrix.argmin(M, 2, 3); "
          "amax = matrix.argmax(M, 2, 3); "
          "cv = mat_var(M, 2, 3, \"col\"); "
          "rs = matrix.std([1, 2, 3, 4, 5, 6], 2, 3, \"row\", 1); "
          "result = round(v * 1000) + round(s * 1000) + amin + amax + "
          "round(cv[0] * 1000) + round(rs[0] * 1000);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix stats Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(7696.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should expose linalg helpers and distribution functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "QR = linalg.qr([1, 0, 0, 1], 2, 2); "
          "Q = QR[0]; R = QR[1]; "
          "P = linalg.pinv([1, 0, 0, 1], 2, 2); "
          "X = linalg.lstsq([1, 0, 0, 1], [7, 9], 2, 2); "
          "SV = linalg.svd([3, 0, 0, 4], 2, 2); "
          "EV = linalg.eig2([2, 0, 0, 3]); "
          "np = round(stats.normal_pdf(0) * 1000); "
          "nc = round(stats.normal_cdf(0) * 1000); "
          "nq = round(stats.normal_quantile(0.5) * 1000); "
          "tp = round(stats.t_pdf(0, 10) * 1000); "
          "tc = round(stats.t_cdf(0, 10) * 1000); "
          "tq = round(stats.t_quantile(0.5, 10) * 1000); "
          "tt = stats.t_test_1samp([1, 2, 3, 4], 2.5); "
          "result = Q[0] + R[3] + P[0] + X[0] + X[1] + SV[0] + SV[1] + "
          "EV[0] + EV[1] + np + nc + nq + tp + tc + tq + tt.df;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Linalg/stats Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(1822.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should expose extended linear algebra helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [4, 2, 2, 3]; "
          "LU = linalg.lu(A, 2); "
          "L = LU[0]; U = LU[1]; P = LU[2]; "
          "X = linalg.lu_solve(A, [8, 8], 2, 1); "
          "C = linalg.solve_cholesky(A, [8, 8], 2, 1); "
          "rank_full = matrix.rank(A, 2, 2); "
          "rank_def = matrix.rank([1, 2, 2, 4], 2, 2); "
          "cond = matrix.cond([2, 0, 0, 4], 2, 2); "
          "E = linalg.eigh([2, 1, 1, 2], 2); "
          "S = linalg.svd([1, 0, 0, 0, 2, 0], 3, 2); "
          "det_lu = linalg.det_lu(A, 2); "
          "SL = matrix.slogdet(A, 2); "
          "RLU = linalg.lu([0, 1, 2, 3], 2, 1); "
          "RP = RLU[2]; "
          "RX = linalg.lu_solve([0, 1, 2, 3], [1, 5], 2, 1, 1); "
          "l0 = L[0]; l1 = L[1]; u0 = U[0]; u3 = U[3]; "
          "p0 = P[0]; p1 = P[1]; "
          "x0 = X[0]; x1 = X[1]; c0 = C[0]; c1 = C[1]; "
          "e0 = E[0]; e1 = E[1]; s0 = S[0]; s1 = S[1]; "
          "sl0 = SL[0]; sl1 = round(SL[1] * 1000); "
          "rp0 = RP[0]; rp1 = RP[1]; rx0 = RX[0]; rx1 = RX[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Extended linalg Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));

      check(fabs((double)(ts_get_num(ctx, "l0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "l1")) - (double)(0.5)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "u0")) - (double)(4.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "u3")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "p0")) - (double)(0.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "p1")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "x0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "x1")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c1")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rank_full")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rank_def")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "cond")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "e0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "e1")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "s0")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "s1")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "det_lu")) - (double)(8.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "sl0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "sl1")) - (double)(2079.0)) <= (double)(1.0));
      check(fabs((double)(ts_get_num(ctx, "rp0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rp1")) - (double)(0.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rx0")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "rx1")) - (double)(1.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should support table select filter groupby and join") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "rows = list(map{id:1, group:\"a\", value:10}, map{id:2, group:\"a\", value:20}, "
          "map{id:3, group:\"b\", value:5}); "
          "right = list(map{id:1, name:\"one\"}, map{id:3, name:\"three\"}); "
          "func keep_big(row) { return row.value > 9; } "
          "sel = table.select(rows, \"id\", \"value\"); "
          "flt = table.filter(rows, \"keep_big\"); "
          "grp = table.groupby(rows, \"group\", \"value\", \"mean\"); "
          "joined = table.join(rows, right, \"id\"); "
          "result = sel.length() + sel[0].value + flt.length() + grp.a.value + grp.b.value + "
          "joined.length() + joined[1].id;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Table Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(40.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

}
