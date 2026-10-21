#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A style attribute is a list of declarations. A custom property keeps its original text. */
static void style_attribute(void) {
  const char * style = "color: RED ! Important; --gap: { 1px  2px } ; @media x { y: z } margin: 0";
  GTEXT_CSS_Tree * tree = NULL;

  if (gtext_css_parse_declaration_list(style, strlen(style), NULL, &tree, NULL) != GTEXT_CSS_OK) {
    return;
  }
  for (size_t i = 0; i < tree->declaration_count; i++) {
    const GTEXT_CSS_Declaration * d = &tree->declarations[i];
    char * value = NULL;
    size_t length = 0;
    if (gtext_css_serialize_values_buffer(d->value, d->value_count, NULL, &value, &length) == GTEXT_CSS_OK) {
      printf("%s = [%s]%s", d->name.data, value, d->important ? " important" : "");
      if (d->original_text.data) {
        printf(", written [%s]", d->original_text.data);
      }
      printf("\n");
      gtext_css_serialize_buffer_free(NULL, value);
    }
  }
  gtext_css_tree_free(tree);
}
