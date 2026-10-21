#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The layer that knows grammars says what is valid; the parse then recovers as the standard says. */
static bool known_property(void * user, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Declaration * d) {
  (void)user;
  (void)parent;
  return strcmp(d->name.data, "color") == 0;
}

static void validity(void) {
  const char * css = "a { color: red; colr: blue; color: green }";
  GTEXT_CSS_Parse_Options opts = gtext_css_parse_options_default();
  opts.validity.declaration = known_property;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result result;

  if (gtext_css_parse_stylesheet(css, strlen(css), &opts, &tree, &result) == GTEXT_CSS_OK) {
    const GTEXT_CSS_Rule * rule = &tree->rules[0];
    printf("%zu declarations kept, %zu parse errors\n", rule->declaration_count,
        gtext_css_result_error_count(&result));
    gtext_css_tree_free(tree);
  }
  gtext_css_result_free(&result);
}
