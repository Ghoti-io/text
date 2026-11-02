#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

/* The caller's element is anything; its style attribute is reached through two functions. */
typedef struct {
  char style[128];
} element;

static GTEXT_CSS_Status get_attribute(void * user, const void * e, const char ** value, size_t * length, bool * present) {
  const element * el = (const element *)e;
  (void)user;
  *value = el->style;
  *length = strlen(el->style);
  *present = el->style[0] != '\0';
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status set_attribute(void * user, const void * e, const char * value, size_t length) {
  element * el = (element *)e;
  (void)user;
  memcpy(el->style, value, length);
  el->style[length] = '\0';
  return GTEXT_CSS_OK;
}

/* An element's style object has no copy of the attribute that can disagree with it: a write goes to the attribute, and a change made behind its back is read on the next call. */
static void cssom_inline_style(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);

  element div;
  strcpy(div.style, "color: red; margin: 1px");
  GTEXT_CSSOM_Attribute_Binding binding;
  memset(&binding, 0, sizeof binding);
  binding.size = sizeof binding;
  binding.get_attribute = get_attribute;
  binding.set_attribute = set_attribute;
  GTEXT_CSSOM_Decl * style = NULL;
  gtext_cssom_inline_style(document, &div, &binding, &style);

  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_decl_set_property(style, "color", 5, "blue", 4, "", 0, &outcome);
  printf("attribute: %s (written: %s)\n", div.style, outcome.attribute_written ? "yes" : "no");
  gtext_cssom_decl_set_property(style, "color", 5, "blue", 4, "", 0, &outcome);
  printf("again: written: %s\n", outcome.attribute_written ? "yes" : "no");

  strcpy(div.style, "width: 5px");   /* setAttribute('style', ...) through the caller's own API */
  char * value = NULL;
  size_t length = 0;
  gtext_cssom_decl_get_property_value(style, "width", 5, &value, &length);
  printf("width: %s\n", value);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), value);
  gtext_cssom_decl_get_property_value(style, "color", 5, &value, &length);
  printf("color: \"%s\"\n", value);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(document), value);

  gtext_cssom_decl_release(style);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
