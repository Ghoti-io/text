#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The layout of this page is a table of one entry: the used width of the element is 49.5px. A real consumer answers for the box it laid out. */
static GTEXT_CSS_Status used_width(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Property_Id property, bool * applies,
    GTEXT_CSS_Value_Set ** out) {
  (void)element;
  (void)pseudo;
  GTEXT_CSS_Property_Value value;
  memset(&value, 0, sizeof value);
  *applies = false;
  GTEXT_CSS_Property_Info info;
  GTEXT_CSS_Registry * registry = (GTEXT_CSS_Registry *)user;
  if (!gtext_css_registry_property_info(registry, property, &info) || strcmp(info.name, "width") != 0) {
    return GTEXT_CSS_OK;   /* does not apply: the resolved value is the computed value */
  }
  if (gtext_css_parse_property_text(registry, "width", 5, "49.5px", 6, NULL, &value) != GTEXT_CSS_OK || !value.set) {
    return GTEXT_CSS_E_INTERNAL;
  }
  *out = value.set;   /* the document takes the set */
  *applies = true;
  return GTEXT_CSS_OK;
}

static void show(GTEXT_CSSOM_Decl * style, const char * property) {
  char * value = NULL;
  size_t length = 0;
  gtext_cssom_decl_get_property_value(style, property, strlen(property), &value, &length);
  GTEXT_CSSOM_Origin origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
  bool found = false;
  gtext_cssom_computed_origin(style, property, strlen(property), &origin, &found);
  printf("%s: %s (%s)\n", property, value, origin == GTEXT_CSSOM_ORIGIN_USED ? "used" : origin == GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT ? "no layout" : "computed");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(gtext_cssom_decl_document(style)), value);
}

/* getComputedStyle(): a live, read-only declaration of resolved values. A color is its used value (a keyword is its sRGB color, `currentcolor` the element's color), a size is
   the used value when the layout gives one and the computed value when it does not, and the declaration reads again when the document or the tree moved. */
static void cssom_computed(void) {
  const char * html = "<!DOCTYPE html><html><head><style>p { color: red; width: 50px; border-top-style: solid } .b { color: blue }</style></head><body><p id=\"a\">x</p></body></html>";
  GTEXT_Markup_Document * markup = NULL;
  GTEXT_HTML_Result result;
  memset(&result, 0, sizeof result);
  gtext_html_parse(html, strlen(html), NULL, &markup, &result);
  gtext_html_result_free(&result);
  GTEXT_CSS_Markup_Adapter * adapter = NULL;
  gtext_css_markup_adapter_new(markup, NULL, &adapter);
  GTEXT_CSS_Selector_Match_Options match = gtext_css_markup_match_options(adapter);

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Layout_Interface layout;
  memset(&layout, 0, sizeof layout);
  layout.size = sizeof layout;
  layout.user = registry;
  layout.used_value = used_width;
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  config.element_interface = gtext_css_markup_interface(adapter);
  config.match_options = &match;
  config.tree_version = gtext_cssom_markup_tree_version;
  config.tree_version_user = markup;
  config.layout = &layout;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Markup_Binding * binding = NULL;
  gtext_cssom_markup_binding_new(document, gtext_cssom_document_scope(document), markup, NULL, &binding);
  gtext_cssom_markup_sync(binding, NULL);

  GTEXT_Markup_Node * root = gtext_markup_document_root(markup);
  GTEXT_Markup_Node * p = gtext_markup_node_next_in_order(root, root, false);
  while (p && !gtext_markup_element_is(p, GTEXT_MARKUP_NS_HTML, "p")) {
    p = gtext_markup_node_next_in_order(root, p, false);
  }
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_computed_style(document, gtext_css_markup_handle(p), NULL, 0, &style);
  show(style, "color");
  show(style, "border-top-color");
  show(style, "width");
  show(style, "margin-top");

  /* the same object, after the tree changed */
  gtext_markup_element_set_attribute(p, NULL, NULL, "class", "b", 1);
  show(style, "color");

  char * text = NULL;
  size_t length = 0;
  gtext_cssom_decl_css_text(style, &text, &length);
  printf("cssText: \"%s\"\n", text);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), text);
  GTEXT_CSSOM_Outcome outcome;
  printf("setProperty: %s\n", gtext_cssom_exception_name(gtext_cssom_decl_set_property(style, "color", 5, "green", 5, "", 0, &outcome)));

  gtext_cssom_decl_release(style);
  gtext_cssom_markup_binding_free(binding);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
  gtext_css_markup_adapter_free(adapter);
  gtext_markup_document_free(markup);
}
