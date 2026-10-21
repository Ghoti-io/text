#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* CSS never fails on content: what is wrong is dropped, and each drop is a parse error. */
static void errors(void) {
  const char * css = "a { color: 'red\n; b c; } d e\n@media x { f: g }";
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, &result) == GTEXT_CSS_OK) {
    for (size_t i = 0; i < gtext_css_result_error_count(&result); i++) {
      GTEXT_CSS_Error e;
      if (gtext_css_result_error_at(&result, i, &e)) {
        printf("%zu:%zu (byte %zu) %s\n", e.line, e.column, e.byte_offset, e.name);
      }
    }
    printf("%zu rules\n", tree->rule_count);
    gtext_css_tree_free(tree);
  }
  gtext_css_result_free(&result);
}
