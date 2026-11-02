#include <ghoti.io/text/css.h>
#include <stdio.h>
#include <string.h>

static GTEXT_CSS_Status on_change(void * user, GTEXT_CSSOM_Media_Query_List * list, bool matches) {
  (void)user;
  (void)list;
  printf("change: matches %s\n", matches ? "yes" : "no");
  return GTEXT_CSS_OK;
}

/* The document as the cascade sees it: a tree scope with an owner sheet and an adopted sheet, the frozen index built from them, and the environment changing. */
static void cssom_document(void) {
  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSS_Media_Environment narrow_env = gtext_css_media_environment_default(NULL);
  narrow_env.viewport.width = 800;
  GTEXT_CSS_Conditions_Options options = gtext_css_conditions_options_default();
  options.media = &narrow_env;
  GTEXT_CSS_Conditions * narrow = NULL;
  gtext_css_conditions_new(registry, &options, &narrow);

  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  config.conditions = narrow;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(document);

  /* document.styleSheets: the owner sheet is listed where its owner node is in the caller's tree */
  const char * text = "a { color: red } @media (min-width: 1000px) { b { width: 1px } }";
  GTEXT_CSSOM_Sheet * owner = NULL;
  gtext_cssom_sheet_new_parsed(document, text, strlen(text), NULL, &owner);
  gtext_cssom_scope_insert_sheet(scope, owner, 0, NULL);
  const GTEXT_CSS_Rule_Index * first = NULL;
  const GTEXT_CSS_Rule_Index * again = NULL;
  gtext_cssom_document_index(document, &first);
  gtext_cssom_document_index(document, &again);
  printf("index: %zu records, asked again: %s object\n", gtext_css_rule_index_record_count(first), again == first ? "the same" : "another");

  /* document.adoptedStyleSheets: a constructed sheet of this document, ordered after the owner sheets */
  GTEXT_CSSOM_Sheet * adopted = NULL;
  gtext_cssom_sheet_new_constructed(document, NULL, &adopted);
  gtext_cssom_sheet_replace_sync(adopted, "c { margin: 0 }", 15, NULL);
  gtext_cssom_scope_adopted_insert(scope, adopted, 0, NULL);
  GTEXT_CSS_Rule_Index * held = NULL;
  gtext_cssom_document_index_retained(document, &held);
  printf("after adopting: %zu records in %zu sheets (the old index is not the new one: %s)\n", gtext_css_rule_index_record_count(held), gtext_css_rule_index_sheet_count(held),
      held != first ? "yes" : "no");

  /* matchMedia, and the environment changing: the listener runs after the change, and the @media rule joins the index; no sheet is compiled again */
  GTEXT_CSSOM_Media_Query_List * wide_query = NULL;
  gtext_cssom_match_media(document, "(min-width: 1000px)", 19, &wide_query);
  gtext_cssom_media_query_list_add_listener(wide_query, on_change, NULL);
  GTEXT_CSS_Sheet * before = NULL;
  gtext_cssom_sheet_snapshot(owner, &before);
  GTEXT_CSS_Media_Environment wide_env = gtext_css_media_environment_default(NULL);
  wide_env.viewport.width = 1200;
  options.media = &wide_env;
  GTEXT_CSS_Conditions * wide = NULL;
  gtext_css_conditions_new(registry, &options, &wide);
  gtext_cssom_document_set_conditions(document, wide, NULL);
  const GTEXT_CSS_Rule_Index * after_index = NULL;
  gtext_cssom_document_index(document, &after_index);
  GTEXT_CSS_Sheet * after = NULL;
  gtext_cssom_sheet_snapshot(owner, &after);
  printf("after the resize: %zu records, the sheet is %s object\n", gtext_css_rule_index_record_count(after_index), after == before ? "the same" : "another");

  gtext_css_sheet_free(before);
  gtext_css_sheet_free(after);
  gtext_css_rule_index_free(held);
  gtext_cssom_media_query_list_release(wide_query);
  gtext_cssom_sheet_release(adopted);
  gtext_cssom_sheet_release(owner);
  gtext_cssom_document_release(document);
  gtext_css_conditions_free(wide);
  gtext_css_conditions_free(narrow);
  gtext_css_registry_free(registry);
}
