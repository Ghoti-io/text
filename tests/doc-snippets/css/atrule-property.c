#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The validity of an @property rule is the answer of the code that registers it. */
static void atrule_property(void) {
  GTEXT_CSS_Registry * registry = NULL;
  if (gtext_css_registry_new(NULL, &registry) != GTEXT_CSS_OK) {
    return;
  }
  const char * rules[] = {"@property --gap { syntax: \"<length>\"; inherits: false; initial-value: 4px }", "@property --gap { syntax: \"<length>\"; inherits: false }",
                          "@property --any { syntax: \"*\"; inherits: true }", "@property gap { syntax: \"*\"; inherits: true }"};
  for (size_t i = 0; i < sizeof rules / sizeof *rules; i++) {
    GTEXT_CSS_Tree * tree = NULL;
    if (gtext_css_parse_stylesheet(rules[i], strlen(rules[i]), NULL, &tree, NULL) != GTEXT_CSS_OK) {
      continue;
    }
    GTEXT_CSS_Atrule_Options options = gtext_css_atrule_options_default();
    GTEXT_CSS_Atrule * rule = NULL;
    GTEXT_CSS_Atrule_Result result;
    if (gtext_css_atrule_parse(registry, &tree->rules[0], &options, &rule, &result) == GTEXT_CSS_OK) {
      printf("rule %zu: %s\n", i + 1, result.valid ? "valid" : "invalid");
    }
    gtext_css_atrule_free(rule);
    gtext_css_tree_free(tree);
  }
  gtext_css_registry_free(registry);
}
