#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* An at-rule is typed data: what the grammar and the rules of the draft refuse is dropped with a diagnostic, and the rest is kept. */
static void atrule_counter_style(void) {
  GTEXT_CSS_Registry * registry = NULL;
  if (gtext_css_registry_new(NULL, &registry) != GTEXT_CSS_OK) {
    return;
  }
  const char * css = "@counter-style roman { system: additive; additive-symbols: 10 X, 5 V, 1 I; range: 10 1; fallback: none; suffix: \". \" }";
  GTEXT_CSS_Tree * tree = NULL;
  if (gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, NULL) == GTEXT_CSS_OK) {
    GTEXT_CSS_Atrule_Options options = gtext_css_atrule_options_default();
    GTEXT_CSS_Atrule * rule = NULL;
    GTEXT_CSS_Atrule_Result result;
    if (gtext_css_atrule_parse(registry, &tree->rules[0], &options, &rule, &result) == GTEXT_CSS_OK && result.valid) {
      printf("%zu descriptors kept, %zu diagnostics\n", gtext_css_atrule_descriptor_count(rule), result.diagnostic_total);
      char * text = NULL;
      size_t length = 0;
      if (gtext_css_atrule_serialize_buffer(rule, NULL, &text, &length) == GTEXT_CSS_OK) {
        printf("%s\n", text);
        gtext_css_atrule_buffer_free(NULL, text);
      }
    }
    gtext_css_atrule_free(rule);
    gtext_css_tree_free(tree);
  }
  gtext_css_registry_free(registry);
}
