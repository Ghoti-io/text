#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_function.h>
#include <stdio.h>
#include <string.h>

/* The function types: a grammar names them (<image>, <easing-function> ...), and a view reads the matched value. */
static void values_functions(void) {
  GTEXT_CSS_Function_Types * types = NULL;
  if (gtext_css_function_types_new(NULL, &types) != GTEXT_CSS_OK) {
    return;
  }
  GTEXT_CSS_Compiled * easing = NULL;
  if (gtext_css_compile("<easing-function>", 17, gtext_css_function_types_resolve, types, NULL, &easing, NULL) == GTEXT_CSS_OK) {
    const char * inputs[] = {"ease-in-out", "steps(4, jump-start)", "linear(0, 1 50%, 0 25%, 1)", "cubic-bezier(0.1, 0.7, 1, 0.1)", "steps(0)"};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
      GTEXT_CSS_Tree * tree = NULL;
      if (gtext_css_parse_component_values(inputs[i], strlen(inputs[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
        continue;
      }
      bool matched = false;
      GTEXT_CSS_Value_Set * set = NULL;
      if (gtext_css_match(easing, tree->values, tree->value_count, NULL, &matched, &set, NULL) == GTEXT_CSS_OK && matched) {
        GTEXT_CSS_Easing * e = NULL;
        bool valid = false;
        if (gtext_css_easing_from_value(gtext_css_value_set_root(set), NULL, &valid, &e) == GTEXT_CSS_OK && valid) {
          char * text = NULL;
          size_t length = 0;
          if (gtext_css_easing_serialize_buffer(e, NULL, &text, &length) == GTEXT_CSS_OK) {
            printf("%s -> %s\n", inputs[i], text);
            gtext_css_serialize_buffer_free(NULL, text);
          }
        }
        else {
          printf("%s -> matches, but is not a valid easing function\n", inputs[i]);
        }
        gtext_css_easing_free(e);
      }
      else {
        printf("%s -> does not match\n", inputs[i]);
      }
      gtext_css_value_set_free(set);
      gtext_css_tree_free(tree);
    }
  }
  gtext_css_compiled_free(easing);
  gtext_css_function_types_free(types);
}
