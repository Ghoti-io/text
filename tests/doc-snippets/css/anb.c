#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* An+B, as :nth-child() holds it: read from component values and written back. */
static void anb(void) {
  const char * inputs[] = {"odd", " -n + 3 ", "2N- 1", "+ 2n"};
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
    GTEXT_CSS_Tree * tree = NULL;
    if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
      continue;
    }
    int64_t a = 0, b = 0;
    char text[44];
    if (gtext_css_anb_parse(tree->values, tree->value_count, &a, &b)) {
      gtext_css_anb_serialize(a, b, text, sizeof text);
      printf("'%s' is A=%lld B=%lld, written %s\n", inputs[i], (long long)a, (long long)b, text);
    }
    else {
      printf("'%s' is not An+B\n", inputs[i]);
    }
    gtext_css_tree_free(tree);
  }
}
