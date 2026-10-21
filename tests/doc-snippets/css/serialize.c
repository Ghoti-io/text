#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Write a tree back out: the text reads back as the same tree. */
static void serialize(void) {
  const char * css = "#\\31 23 > .a\\ b { width: calc( 1px+2PX ); content: '\\'q\\'' ; } /* gone */";
  GTEXT_CSS_Tree * tree = NULL;

  if (gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, NULL) != GTEXT_CSS_OK) {
    return;
  }
  char * text = NULL;
  size_t length = 0;
  if (gtext_css_serialize_tree_buffer(tree, NULL, &text, &length) == GTEXT_CSS_OK) {
    printf("%s\n", text);
    gtext_css_serialize_buffer_free(NULL, text);
  }
  GTEXT_CSS_Serialize_Options opts = gtext_css_serialize_options_default();
  opts.numbers = GTEXT_CSS_NUMBERS_CSSOM;
  const GTEXT_CSS_Declaration * width = &tree->rules[0].declarations[0];
  if (gtext_css_serialize_values_buffer(width->value, width->value_count, &opts, &text, &length) == GTEXT_CSS_OK) {
    printf("%s\n", text);
    gtext_css_serialize_buffer_free(&opts, text);
  }
  gtext_css_tree_free(tree);
}
