#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The validity callbacks make a parse drop the declarations that CSS says it must, and the descriptors are those of the at-rule. */
static void registry_validity(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Property_Validity validity;
  gtext_css_property_validity_init(&validity, registry, NULL);

  GTEXT_CSS_Parse_Options options = gtext_css_parse_options_default();
  options.validity = gtext_css_property_validity(&validity);
  const char * css = "@font-face { font-display: swap; width: 1px } a { color: red; font-display: swap; width: -1px; nosuch: 1 }";
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_stylesheet(css, strlen(css), &options, &tree, NULL);
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_tree_buffer(tree, NULL, &text, &length);
  printf("%s\n", text);
  printf("judged %zu, dropped %zu\n", validity.checked, validity.rejected);
  gtext_css_serialize_buffer_free(NULL, text);
  gtext_css_tree_free(tree);
  gtext_css_registry_free(registry);
}
