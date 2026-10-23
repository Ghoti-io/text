#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Math functions: read as a typed tree, simplified as the draft says, and written as a specified value is. */
static void values_math(void) {
  const char * inputs[] = {"calc(1px + 2px)", "calc(20px + 2em)", "calc(10% - 5px)", "min(20px, 10%, 3em)",
      "calc(2 * 3deg)", "calc(1px * 2px)", "calc(infinity * 1px)", "mod(-7px, 3px)", "round(up, 7px, 2px)"};
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
    GTEXT_CSS_Tree * tree = NULL;
    if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
      continue;
    }
    GTEXT_CSS_Math * math = NULL;
    bool valid = false;
    if (gtext_css_math_parse(&tree->values[0], NULL, NULL, &valid, &math, NULL) == GTEXT_CSS_OK && valid) {
      char * text = NULL;
      size_t length = 0;
      if (gtext_css_math_serialize_buffer(math, NULL, &text, &length) == GTEXT_CSS_OK) {
        printf("%s -> %s\n", inputs[i], text);
        gtext_css_serialize_buffer_free(NULL, text);
      }
    }
    else {
      printf("%s -> not a valid calculation\n", inputs[i]);
    }
    gtext_css_math_free(math);
    gtext_css_tree_free(tree);
  }
}
