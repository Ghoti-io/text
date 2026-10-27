#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static void cc_add(GTEXT_CSS_Candidates * set, GTEXT_CSS_Origin origin, const char * name, const char * value, unsigned order) {
  GTEXT_CSS_Cascade_Entry keys;
  memset(&keys, 0, sizeof keys);
  keys.origin = origin;
  keys.sheet = order;
  gtext_css_candidates_add_text(set, &keys, name, strlen(name), value, strlen(value), NULL, NULL, NULL);
}

static void cc_show(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Value * value = gtext_css_computed_root(style, gtext_css_registry_find_property(registry, name, strlen(name)));
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

/* The cascade's winners are a declaration block, which the computed-value layer takes as it is. The logical and physical margins of the element share a
   slot, so the later declaration wins whichever name it is written with, and the author's `revert` gives the user's color back. */
static void cascade_computed(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, NULL, &context, NULL);

  GTEXT_CSS_Candidates * set = NULL;
  gtext_css_candidates_new(registry, NULL, NULL, &set);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "direction", "rtl", 0);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "margin-right", "20px", 1);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "margin-inline-start", "10px", 2);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "margin-left", "5px", 3);
  cc_add(set, GTEXT_CSS_ORIGIN_USER, "color", "green", 4);
  cc_add(set, GTEXT_CSS_ORIGIN_AUTHOR, "color", "revert", 5);

  GTEXT_CSS_Cascaded * cascaded = NULL;
  gtext_css_cascade(set, NULL, &cascaded, NULL);
  GTEXT_CSS_Declaration_Block * block = NULL;
  gtext_css_cascaded_to_block(cascaded, NULL, &block);
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_compute_style(context, block, NULL, NULL, &style, NULL);
  cc_show(registry, style, "color");
  cc_show(registry, style, "margin-left");
  cc_show(registry, style, "margin-right");

  gtext_css_computed_free(style);
  gtext_css_block_free(block);
  gtext_css_cascaded_free(cascaded);
  gtext_css_candidates_free(set);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
