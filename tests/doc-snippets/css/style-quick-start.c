#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The caller's tree: an element is whatever the caller says it is. Here, a name and its attributes. */
typedef struct {
  const char * name;
  const char * attributes[8][2];
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
  for (size_t i = 0; i < 8 && n->attributes[i][0]; i++) {
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

/* One call turns an element into a computed style: its presentation attributes (SVG's `stroke-width="2"`, a number being user units), its `style`
   attribute, and the style of the element it inherits from. A declaration that is not valid is dropped and counted, as CSS says. */
static void style_quick_start(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);

  node group = {"g", {{"font-size", "20"}, {"stroke-width", "1"}}};
  node rect = {"rect", {{"stroke-width", "2"}, {"opacity", "0.5"}, {"stroke-linecap", "wavy"}, {"style", "letter-spacing: 0.5em; bogus: 1; stroke-width: 5"}}};

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

  GTEXT_CSS_Computed_Style *parent = NULL, *style = NULL;
  gtext_css_style_compute(context, &group, NULL, NULL, 0, NULL, &parent, NULL);
  gtext_css_style_compute(context, &rect, parent, NULL, 0, NULL, &style, NULL);

  show(registry, style, "font-size");
  show(registry, style, "stroke-width");
  show(registry, style, "letter-spacing");
  show(registry, style, "opacity");

  GTEXT_CSS_Cascade_Entry where;
  gtext_css_computed_specified(style, gtext_css_registry_find_property(registry, "stroke-width", 12), &where);
  printf("stroke-width came from the %s\n", where.element_attached ? "style attribute" : where.presentational_hint ? "presentation attribute" : "a rule");
  gtext_css_computed_specified(style, gtext_css_registry_find_property(registry, "opacity", 7), &where);
  printf("opacity came from the %s\n", where.element_attached ? "style attribute" : where.presentational_hint ? "presentation attribute" : "a rule");

  for (size_t i = 0; i < gtext_css_computed_diagnostic_total(style); i++) {
    GTEXT_CSS_Style_Diagnostic d;
    gtext_css_computed_diagnostic(style, i, &d);
    printf("dropped %.*s at byte %zu\n", (int)d.name.length, d.name.data, d.offset);
  }

  gtext_css_computed_free(style);
  gtext_css_computed_free(parent);
  gtext_css_compute_context_free(context);
  gtext_css_registry_free(registry);
}
