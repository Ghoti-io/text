#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A match is bounded in work and memory, and a bound that is reached is a status, never "no match". */
static void values_limits(void) {
  GTEXT_CSS_Compiled * compiled = NULL;
  const char * grammar = "[ [ a? b? ]* ]+";
  gtext_css_compile(grammar, strlen(grammar), NULL, NULL, NULL, &compiled, NULL);
  char css[4000];
  size_t n = 0;
  for (int i = 0; i < 1000; i++) {
    n += (size_t)snprintf(css + n, sizeof css - n, "a ");
  }
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_component_values(css, n, NULL, &tree, NULL);

  GTEXT_CSS_Match_Options options = gtext_css_match_options_default();
  options.max_match_work = 100;
  bool matched = false;
  GTEXT_CSS_Match_Result result;
  GTEXT_CSS_Status status = gtext_css_match(compiled, tree->values, tree->value_count, &options, &matched, NULL, &result);
  printf("with 100 units of work: %s\n", gtext_css_status_string(status));

  options.max_match_work = 0; /* the default */
  status = gtext_css_match(compiled, tree->values, tree->value_count, &options, &matched, NULL, &result);
  printf("by default: %s, matched %s, %zu positions\n", gtext_css_status_string(status), matched ? "yes" : "no", result.positions);
  printf("work for 1000 tokens: %zu units\n", result.work);
  gtext_css_tree_free(tree);
  gtext_css_compiled_free(compiled);
}
