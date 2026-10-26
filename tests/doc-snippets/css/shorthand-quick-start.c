#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* A shorthand is declared once and stored as its longhands; the block writes it back as the shortest shorthand its longhands make. */
static void shorthand_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);

  const char * declarations[][2] = {{"margin", "1px 2px"}, {"border", "red solid 2px"}, {"margin-top", "5px"}, {"flex", "2"}};
  for (size_t i = 0; i < sizeof declarations / sizeof declarations[0]; i++) {
    gtext_css_block_add_text(block, declarations[i][0], strlen(declarations[i][0]), declarations[i][1], strlen(declarations[i][1]), false, NULL, NULL, NULL);
  }
  printf("%zu longhands\n", gtext_css_block_count(block));

  char * text = NULL;
  size_t length = 0;
  gtext_css_block_serialize_buffer(block, &text, &length);
  printf("%s\n", text);
  gtext_css_serialize_buffer_free(NULL, text);

  const char * asked[] = {"margin", "margin-right", "border-top-style", "grid"};
  for (size_t i = 0; i < sizeof asked / sizeof asked[0]; i++) {
    gtext_css_block_get_property_value(block, gtext_css_registry_find_property(registry, asked[i], strlen(asked[i])), &text, &length, NULL);
    printf("%s -> \"%s\"\n", asked[i], text);
    gtext_css_serialize_buffer_free(NULL, text);
  }
  gtext_css_block_free(block);
  gtext_css_registry_free(registry);
}
