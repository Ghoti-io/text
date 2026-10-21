#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Parse a stylesheet: a rule with a declaration, a nested rule, and the declarations after it. */
static void quick_start(void) {
  const char * css = "a, b { color: red; &:hover { color: blue } margin: 0 !important }";
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, &result) != GTEXT_CSS_OK) {
    fprintf(stderr, "not parsed: %s\n", result.message);
    gtext_css_result_free(&result);
    return;
  }
  const GTEXT_CSS_Rule * rule = &tree->rules[0];
  printf("%zu rule: %zu declaration, %zu child rules\n", tree->rule_count, rule->declaration_count,
      rule->rule_count);
  printf("%s: %s\n", rule->declarations[0].name.data, rule->declarations[0].value[0].value.data);
  for (size_t i = 0; i < rule->rule_count; i++) {
    const GTEXT_CSS_Rule * child = &rule->rules[i];
    if (child->kind == GTEXT_CSS_RULE_NESTED_DECLARATIONS) {
      printf("then %s%s\n", child->declarations[0].name.data,
          child->declarations[0].important ? " !important" : "");
    }
    else {
      printf("nested rule at line %zu, column %zu\n", child->span.line, child->span.column);
    }
  }
  gtext_css_tree_free(tree);
  gtext_css_result_free(&result);
}
