#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/markup/markup_query.h>
#include <stdio.h>
#include <string.h>

/* A style sheet is compiled once and shared; an index merges the sheets of a document; for each element the index adds to the cascade the declarations of the rules that match
   it, and the cascade says which declaration wins each property. Here the element is a node of a Markup document. */
static void sheet_quick_start(void) {
  static const char html[] = "<!DOCTYPE html><div class=card><p id=a class=lead>one</p><p id=b>two</p></div>";
  static const char css[] =
      "p { color: red; margin-top: 1px }\n"
      ".card > .lead { color: green }\n"
      "@layer base { p { margin-top: 5px; margin-left: 5px } }\n"
      "#b { margin-top: 9px !important }\n";

  GTEXT_CSS_Registry * registry = NULL;
  gtext_css_registry_new(NULL, &registry);

  GTEXT_Markup_Document * document = NULL;
  GTEXT_HTML_Result html_result;
  memset(&html_result, 0, sizeof html_result);
  gtext_html_parse(html, strlen(html), NULL, &document, &html_result);
  gtext_html_result_free(&html_result);

  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result parsed;
  memset(&parsed, 0, sizeof parsed);
  gtext_css_parse_stylesheet(css, strlen(css), NULL, &tree, &parsed);
  GTEXT_CSS_Sheet * sheet = NULL;
  gtext_css_sheet_compile(tree, registry, NULL, &sheet, NULL);
  gtext_css_tree_free(tree);
  gtext_css_result_free(&parsed);

  GTEXT_CSS_Markup_Adapter * adapter = NULL;
  gtext_css_markup_adapter_new(document, NULL, &adapter);

  GTEXT_CSS_Rule_Index_Options index_options = gtext_css_markup_rule_index_options(adapter);
  GTEXT_CSS_Rule_Index * index = NULL;
  gtext_css_rule_index_new(registry, &index_options, &index);
  GTEXT_CSS_Sheet_Registration registration;
  memset(&registration, 0, sizeof registration);
  registration.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  gtext_css_rule_index_add_sheet(index, sheet, &registration, NULL);
  gtext_css_rule_index_freeze(index, NULL);

  GTEXT_CSS_Selector_Match_Options options = gtext_css_markup_match_options(adapter);
  GTEXT_CSS_Selector_Match_Context * match = NULL;
  gtext_css_selector_match_context_new(gtext_css_markup_interface(adapter), &options, &match);

  GTEXT_Markup_Node ** paragraphs = NULL;
  size_t count = 0;
  gtext_markup_query_selector_all(gtext_markup_document_root(document), "p", 1, NULL, &paragraphs, &count, NULL);
  for (size_t e = 0; e < count; e++) {
    GTEXT_CSS_Candidates * set = NULL;
    gtext_css_candidates_new(registry, NULL, NULL, &set);
    gtext_css_rules_collect(index, match, gtext_css_markup_handle(paragraphs[e]), NULL, set, NULL);
    GTEXT_CSS_Cascaded * cascaded = NULL;
    gtext_css_cascade(set, NULL, &cascaded, NULL);
    GTEXT_Markup_Attribute id;
    gtext_markup_element_get_attribute(paragraphs[e], NULL, "id", &id);
    for (size_t i = 0; i < gtext_css_cascaded_count(cascaded); i++) {
      GTEXT_CSS_Cascade_Entry winner;
      gtext_css_cascaded_at(cascaded, i, &winner, NULL);
      GTEXT_CSS_Property_Info info;
      gtext_css_registry_property_info(registry, winner.property, &info);
      char * value = NULL;
      size_t length = 0;
      gtext_css_serialize_value_buffer(gtext_css_value_set_root(winner.set), NULL, &value, &length);
      printf("%.*s %s: %s\n", (int)id.value_length, id.value, info.name, value);
      gtext_css_serialize_buffer_free(NULL, value);
    }
    gtext_css_cascaded_free(cascaded);
    gtext_css_candidates_free(set);
  }
  gtext_markup_query_free(NULL, paragraphs);
  gtext_css_selector_match_context_free(match);
  gtext_css_markup_adapter_free(adapter);
  gtext_css_rule_index_free(index);
  gtext_css_sheet_free(sheet);
  gtext_markup_document_free(document);
  gtext_css_registry_free(registry);
}
