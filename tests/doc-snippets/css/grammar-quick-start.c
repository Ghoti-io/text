#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* Compile a property's grammar, match a declaration's value against it, and read the typed result. */
static void grammar_quick_start(void) {
  const char * grammar = "[ <length> | <percentage> | auto ]{1,4}";
  GTEXT_CSS_Compiled * compiled = NULL;
  GTEXT_CSS_Grammar_Result built;
  if (gtext_css_compile(grammar, strlen(grammar), NULL, NULL, NULL, &compiled, &built) != GTEXT_CSS_OK) {
    printf("not compiled: %s %s\n", built.message, built.name);
    return;
  }
  const char * css = "10PX auto 5%";
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_component_values(css, strlen(css), NULL, &tree, NULL);

  bool matched = false;
  GTEXT_CSS_Value_Set * set = NULL;
  gtext_css_match(compiled, tree->values, tree->value_count, NULL, &matched, &set, NULL);
  const GTEXT_CSS_Value * list = gtext_css_value_set_root(set);
  printf("matched: %s, a %s of %zu\n", matched ? "yes" : "no", gtext_css_val_kind_name(list->kind), list->child_count);
  for (size_t i = 0; i < list->child_count; i++) {
    const GTEXT_CSS_Value * v = &list->children[i];
    printf("  %s", gtext_css_val_kind_name(v->kind));
    if (v->kind == GTEXT_CSS_VAL_DIMENSION) {
      printf(" %g %s", v->number, gtext_css_unit_name(v->unit));
    }
    printf("\n");
  }
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(list, NULL, &text, &length);
  printf("written: %s\n", text);
  gtext_css_serialize_buffer_free(NULL, text);

  gtext_css_value_set_free(set);
  gtext_css_tree_free(tree);
  gtext_css_compiled_free(compiled);
}
