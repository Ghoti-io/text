#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static const char * kind_name(GTEXT_CSSOM_Event_Kind kind) {
  switch (kind) {
  case GTEXT_CSSOM_EVENT_RULE_INSERTED: return "rule inserted";
  case GTEXT_CSSOM_EVENT_RULE_REMOVED: return "rule removed";
  case GTEXT_CSSOM_EVENT_RULE_REPLACED: return "rule replaced";
  case GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED: return "declarations changed";
  default: return "other";
  }
}

/* What an invalidation set needs, read from the event: where the rule is, its selectors before and after, and which properties changed. */
static GTEXT_CSS_Status observe(void * user, const GTEXT_CSSOM_Event * event) {
  const GTEXT_Allocator * allocator = (const GTEXT_Allocator *)user;
  printf("%s", kind_name(event->kind));
  if (event->has_path) {
    printf(" at");
    for (size_t i = 0; i < event->path_length; i++) {
      printf(" %zu", event->path[i]);
    }
  }
  char * text = NULL;
  size_t length = 0;
  if (event->selectors_before && gtext_css_selector_serialize_buffer(event->selectors_before, allocator, &text, &length) == GTEXT_CSS_OK) {
    printf(", was %s", text);
    gtext_css_selector_buffer_free(allocator, text);
  }
  if (event->selectors && gtext_css_selector_serialize_buffer(event->selectors, allocator, &text, &length) == GTEXT_CSS_OK) {
    printf(", selectors %s (class %s, attribute %s)", text, (event->selector_dependencies.kinds & GTEXT_CSS_DEP_CLASS) ? "yes" : "no",
        (event->selector_dependencies.kinds & GTEXT_CSS_DEP_ATTR) ? "yes" : "no");
    gtext_css_selector_buffer_free(allocator, text);
  }
  if (event->property_count) {
    printf(", %zu properties", event->property_count);
  }
  printf(", detached %s\n", event->detached_since ? "yes" : "no");
  return GTEXT_CSS_OK;
}

/* Observers: one event for each mutation that changed something, after the state is consistent; a batch defers them to its end. */
static void cssom_events(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  const GTEXT_Allocator * allocator = gtext_cssom_document_allocator(document);

  const char * text = "a { color: red } @media screen { b { width: 1px } }";
  GTEXT_CSSOM_Sheet * sheet = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &sheet);
  GTEXT_CSSOM_Observer_Handle handle = 0;
  gtext_cssom_document_observe(document, observe, (void *)allocator, &handle);

  gtext_cssom_sheet_insert_rule(sheet, "ul > li.on[title] { margin: 0 }", 31, 0, NULL, NULL);
  const GTEXT_CSSOM_Rule_List * rules = NULL;
  gtext_cssom_sheet_rules(sheet, &rules);
  GTEXT_CSSOM_Rule * first = gtext_cssom_rule_list_at(rules, 0);
  gtext_cssom_rule_set_selector_text(first, "p, q", 4, NULL);

  /* a batch: nothing is delivered until the outermost one ends, in the order the mutations were made */
  gtext_cssom_document_begin_batch(document);
  gtext_cssom_sheet_delete_rule(sheet, 0, NULL);
  GTEXT_CSSOM_Decl * style = NULL;
  GTEXT_CSSOM_Rule * second = gtext_cssom_rule_list_at(rules, 0);
  gtext_cssom_rule_style(second, &style);
  gtext_cssom_decl_set_property(style, "color", 5, "blue", 4, "", 0, NULL);
  printf("in the batch: nothing yet\n");
  gtext_cssom_document_end_batch(document, NULL);

  gtext_cssom_decl_release(style);
  gtext_cssom_rule_release(second);
  gtext_cssom_rule_release(first);
  gtext_cssom_document_unobserve(document, handle);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
}
