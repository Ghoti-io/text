#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Colors: the declared value, as CSS Color 4 and 5 serialize it. */
static void values_color(void) {
  const char * inputs[] = {"RED", "#ff000080", "rgb(255 0 0 / 50%)", "hsl(120deg 100% 50%)", "rgb(none 0 0)",
      "oklch(0.5 0.1 120deg)", "color-mix(in srgb, red 30%, blue)", "rgb(from red r g b / 0.5)", "rgb(1 2)"};
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
    GTEXT_CSS_Tree * tree = NULL;
    if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
      continue;
    }
    GTEXT_CSS_Color * color = NULL;
    bool valid = false;
    if (gtext_css_color_parse(&tree->values[0], NULL, &valid, &color, NULL) == GTEXT_CSS_OK && valid) {
      char * text = NULL;
      size_t length = 0;
      if (gtext_css_color_serialize_buffer(color, NULL, &text, &length) == GTEXT_CSS_OK) {
        printf("%s -> %s\n", inputs[i], text);
        gtext_css_serialize_buffer_free(NULL, text);
      }
    }
    else {
      printf("%s -> not a color\n", inputs[i]);
    }
    gtext_css_color_free(color);
    gtext_css_tree_free(tree);
  }
}
