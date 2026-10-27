#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* An `@property` rule of a style sheet is a registration too. The rules of the sheets are collected into a set, and a compute context made with the set
   computes the registered properties by them. A rule that is not valid is ignored, and the set says which and where; a registration the caller made
   with gtext_css_registry_register_custom() wins over a rule of the same name. */
static void variables_property_rule(void) {
  const char sheet[] =
      "@property --size { syntax: '<length>'; inherits: true; initial-value: 16px }\n"
      "@property --broken { inherits: true }\n";
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Tree * tree = NULL;
  gtext_css_parse_stylesheet(sheet, strlen(sheet), NULL, &tree, NULL);

  GTEXT_CSS_At_Property_Set * rules = NULL;
  gtext_css_at_property_set_new(registry, NULL, &rules);
  gtext_css_at_property_collect(rules, tree, NULL);
  printf("%zu rule registered\n", gtext_css_at_property_count(rules));
  for (size_t i = 0; i < gtext_css_at_property_diagnostic_total(rules); i++) {
    GTEXT_CSS_At_Property_Diagnostic d;
    gtext_css_at_property_diagnostic(rules, i, &d);
    printf("ignored %.*s: %s\n", (int)d.name.length, d.name.data, gtext_css_at_property_reason_name(d.reason));
  }
  GTEXT_CSS_Custom_Info info;
  if (gtext_css_at_property_find(rules, "--size", 6, &info)) {
    printf("--size: syntax %.*s, initial %.*s\n", (int)info.syntax_length, info.syntax, (int)info.initial_length, info.initial_value);
  }

  GTEXT_CSS_Compute_Options options = gtext_css_compute_options_default();
  options.property_rules = rules;
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, &options, &context, NULL);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_block_new(registry, NULL, &block);
  gtext_css_block_add_text(block, "width", 5, "var(--size)", 11, false, NULL, NULL, NULL);
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_compute_style(context, block, NULL, NULL, &style, NULL);
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(gtext_css_computed_root(style, gtext_css_registry_find_property(registry, "width", 5)), NULL, &text, &length);
  printf("width: %s\n", text);
  gtext_css_serialize_buffer_free(NULL, text);

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_compute_context_free(context);
  gtext_css_at_property_set_free(rules);
  gtext_css_tree_free(tree);
  gtext_css_registry_free(registry);
}
