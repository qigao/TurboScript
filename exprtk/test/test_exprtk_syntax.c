#include "tinytest.h"

#include <string.h>

#include "exprtk.h"

suite("ExprTk Syntax") {
  group("parse lifecycle") {
    it("parses and frees a valid syntax tree without an evaluator") {
      const char *source = "answer = 6 * 7;";
      exprtk_node_t *root = exprtk_parse(source, strlen(source));

      check_not_null(root);
      exprtk_free(root);
    }
  }
}
