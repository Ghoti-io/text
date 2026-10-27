#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char * name;
  const char * attributes[4][2];
} node;

static bool node_local_name(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out) {
  (void)user;
  out->data = ((const node *)element)->name;
  out->length = strlen(out->data);
  return true;
}

static bool node_namespace(void * user, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out) {
  (void)user;
  (void)element;
  out->data = GTEXT_CSS_SVG_NAMESPACE;
  out->length = strlen(out->data);
  return true;
}

static bool node_attribute(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
  (void)user;
  (void)ns;
  const node * n = (const node *)element;
  for (size_t i = 0; i < 4 && n->attributes[i][0]; i++) {
    if (strlen(n->attributes[i][0]) == name->length && memcmp(n->attributes[i][0], name->data, name->length) == 0) {
      out->data = n->attributes[i][1];
      out->length = strlen(out->data);
      return true;
    }
  }
  return false;
}

static void show(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Value * value = gtext_css_computed_root(style, gtext_css_registry_find_property(registry, name, strlen(name)));
  char * text = NULL;
  size_t length = 0;
  gtext_css_serialize_value_buffer(value, NULL, &text, &length);
  printf("%s: %s\n", name, text);
  gtext_css_serialize_buffer_free(NULL, text);
}

static GTEXT_CSS_Style_Entry rule(const char * declarations, uint32_t order, unsigned specificity) {
  GTEXT_CSS_Style_Entry entry;
  memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  entry.keys.rule = order;
  entry.keys.specificity.b = specificity;
  entry.text.data = declarations;
  entry.text.length = strlen(declarations);
  return entry;
}

/* The declarations of the style rules that matched an element (once selectors exist, a matcher supplies them) are entries with the keys the cascade sorts by.
   A presentation attribute is under every rule, whatever its specificity; the `style` attribute is over every rule that is not `!important`. */
static void style_entries(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Element_Interface table;
  memset(&table, 0, sizeof table);
  table.size = sizeof table;
  table.local_name = node_local_name;
  table.namespace_uri = node_namespace;
  table.attribute = node_attribute;
  GTEXT_CSS_Compute_Options options = gtext_css_compute_options_default();
  options.elements = &table;
  GTEXT_CSS_Compute_Context * context = NULL;
  gtext_css_compute_context_new(registry, NULL, &options, &context, NULL);

  node rect = {"rect", {{"stroke-width", "2"}, {"opacity", "0.25"}, {"font-size", "10"}, {"style", "font-size: 30px"}}};
  const GTEXT_CSS_Style_Entry rules[] = {
      rule("stroke-width: 4px; opacity: 0.5", 0, 0),
      rule("opacity: 0.75", 1, 10),            /* more specific: wins over the first */
      rule("font-size: 99px", 2, 0),           /* loses to the style attribute */
  };
  GTEXT_CSS_Computed_Style * style = NULL;
  gtext_css_style_compute(context, &rect, NULL, rules, 3, NULL, &style, NULL);
  show(registry, style, "stroke-width");
  show(registry, style, "opacity");
  show(registry, style, "font-size");

  gtext_css_computed_free(style);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
