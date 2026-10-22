#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A stylesheet's bytes: the encoding is found as CSS Syntax 3.2 says, and every position is in the bytes. */
static void bytes(void) {
  /* "@charset "windows-1251"; .привет { color: red }", in windows-1251 */
  const char css[] = "@charset \"windows-1251\"; .\xef\xf0\xe8\xe2\xe5\xf2 { color: red }";
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet_bytes(css, sizeof css - 1, NULL, NULL, &tree, &result) != GTEXT_CSS_OK) {
    gtext_css_result_free(&result);
    return;
  }
  const GTEXT_CSS_Rule * rule = &tree->rules[1];
  const GTEXT_CSS_Component_Value * name = &rule->prelude[1];
  printf("read as %s (%s)\n", gtext_encoding_name(result.encoding), gtext_css_encoding_source_name(result.encoding_source));
  printf("%s is %zu bytes of text and %zu of the stylesheet\n", name->value.data, name->value.length,
      name->span.end - name->span.offset);
  printf("the rule is bytes %zu to %zu of %zu\n", rule->span.offset, rule->span.end, result.input_bytes);
  gtext_css_tree_free(tree);
  gtext_css_result_free(&result);
}
