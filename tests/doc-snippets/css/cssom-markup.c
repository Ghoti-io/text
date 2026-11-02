#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A Markup document's <style> elements are the owner sheets of the scope: one sync makes the list say what the elements say, and an unchanged tree costs no walk. */
static void cssom_markup(void) {
  const char * html = "<!DOCTYPE html><html><head><style>p { color: red }</style><style type=\"text/plain\">q { }</style></head><body><style>div { width: 1px }</style></body></html>";
  GTEXT_Markup_Document * markup = NULL;
  GTEXT_HTML_Result result;
  memset(&result, 0, sizeof result);
  gtext_html_parse(html, strlen(html), NULL, &markup, &result);
  gtext_html_result_free(&result);

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);
  GTEXT_CSSOM_Config config = gtext_cssom_config_default();
  config.registry = registry;
  config.tree_version = gtext_cssom_markup_tree_version;   /* the Markup document's generation: an edit of an element's style attribute is seen */
  config.tree_version_user = markup;
  GTEXT_CSSOM_Document * document = NULL;
  gtext_cssom_document_new(&config, &document);
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(document);

  GTEXT_CSSOM_Markup_Binding * binding = NULL;
  gtext_cssom_markup_binding_new(document, scope, markup, NULL, &binding);
  gtext_cssom_markup_sync(binding, NULL);
  printf("sheets: %zu (the style with type text/plain has none)\n", gtext_cssom_scope_sheet_count(scope));

  GTEXT_CSSOM_Markup_Counters before, after;
  gtext_cssom_markup_binding_counters(binding, &before);
  gtext_cssom_markup_sync(binding, NULL);
  gtext_cssom_markup_binding_counters(binding, &after);
  printf("sync of an unchanged tree: walks %llu\n", (unsigned long long)(after.walks - before.walks));

  /* a change of an element's text is a new sheet; the other is kept */
  GTEXT_Markup_Node * root = gtext_markup_document_root(markup);
  GTEXT_Markup_Node * node = gtext_markup_node_next_in_order(root, root, false);
  while (node && !gtext_markup_element_is(node, GTEXT_MARKUP_NS_HTML, "style")) {
    node = gtext_markup_node_next_in_order(root, node, false);
  }
  GTEXT_CSSOM_Sheet * old_sheet = gtext_cssom_markup_sheet_of(binding, node);
  GTEXT_Markup_Node * old_text = gtext_markup_node_first_child(node);
  gtext_markup_node_set_data(old_text, "p { color: blue }", 17);
  GTEXT_CSSOM_Outcome outcome;
  gtext_cssom_markup_sync(binding, &outcome);
  GTEXT_CSSOM_Sheet * new_sheet = gtext_cssom_markup_sheet_of(binding, node);
  printf("after the text change: %s sheet, the old sheet has %s owner, outcome %s\n", new_sheet != old_sheet ? "a new" : "the same", gtext_cssom_sheet_owner_node(old_sheet) ? "an" : "no",
      outcome.kind == GTEXT_CSSOM_CHANGED ? "changed" : "unchanged");

  gtext_cssom_sheet_release(new_sheet);
  gtext_cssom_sheet_release(old_sheet);
  gtext_cssom_markup_binding_free(binding);
  gtext_cssom_document_release(document);
  gtext_css_registry_free(registry);
  gtext_markup_document_free(markup);
}
