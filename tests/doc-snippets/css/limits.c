#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A limit is its own status, with no tree, and says where it was reached. */
static void limits(void) {
  const char * css = "a { b { c {\n d { } } } }";
  GTEXT_CSS_Parse_Options opts = gtext_css_parse_options_default();
  opts.max_depth = 3;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  GTEXT_CSS_Status status = gtext_css_parse_stylesheet(css, strlen(css), &opts, &tree, &result);
  printf("%s at %zu:%zu, tree %s\n", gtext_css_status_string(status), result.failure.line,
      result.failure.column, tree ? "made" : "not made");
  gtext_css_result_free(&result);

  opts.max_depth = 4;
  status = gtext_css_parse_stylesheet(css, strlen(css), &opts, &tree, &result);
  printf("%s with depth 4\n", gtext_css_status_string(status));
  gtext_css_tree_free(tree);
  gtext_css_result_free(&result);
}
