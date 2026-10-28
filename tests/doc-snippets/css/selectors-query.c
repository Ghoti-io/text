#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/markup/markup_query.h>
#include <stdio.h>
#include <string.h>

static void sq_print(const char * label, GTEXT_Markup_Node * const * nodes, size_t count) {
  printf("%s:", label);
  for (size_t i = 0; i < count; i++) {
    GTEXT_Markup_Attribute id;
    if (gtext_markup_element_get_attribute(nodes[i], NULL, "id", &id)) {
      printf(" %.*s", (int)id.value_length, id.value);
    }
  }
  printf("\n");
}

/* The queries of the DOM on a Markup document: all the elements a selector matches in document order, whether one element matches, and the nearest inclusive ancestor that does.
   A selector that is not valid is GTEXT_MARKUP_E_SELECTOR and the result says where. */
static void selectors_query(void) {
  static const char html[] =
      "<!DOCTYPE html><ul id=list><li id=one class=x>1</li><li id=two>2</li><li id=three class=x><b id=bold>3</b></li></ul>";
  GTEXT_Markup_Document * document = NULL;
  GTEXT_HTML_Result parsed;
  memset(&parsed, 0, sizeof parsed);
  gtext_html_parse(html, strlen(html), NULL, &document, &parsed);
  gtext_html_result_free(&parsed);
  const GTEXT_Markup_Node * root = gtext_markup_document_root(document);

  GTEXT_Markup_Node ** found = NULL;
  size_t count = 0;
  gtext_markup_query_selector_all(root, "li.x", 4, NULL, &found, &count, NULL);
  sq_print("li.x", found, count);
  gtext_markup_query_free(NULL, found);

  gtext_markup_query_selector_all(root, "ul:has(> li:nth-child(odd of .x)) > li:not(.x) + li", 51, NULL, &found, &count, NULL);
  sq_print("odd .x", found, count);
  gtext_markup_query_free(NULL, found);

  GTEXT_Markup_Node * bold = NULL;
  gtext_markup_query_selector(root, "b", 1, NULL, &bold, NULL);
  GTEXT_Markup_Node * nearest = NULL;
  gtext_markup_closest(bold, "li", 2, NULL, &nearest, NULL);
  GTEXT_Markup_Node * list[1] = {nearest};
  sq_print("closest li", list, 1);
  bool is_x = false;
  gtext_markup_matches(nearest, ".x", 2, NULL, &is_x, NULL);
  printf("matches .x: %s\n", is_x ? "yes" : "no");

  GTEXT_Markup_Query_Result result;
  memset(&result, 0, sizeof result);
  const GTEXT_Markup_Status status = gtext_markup_query_selector(root, "li:", 3, NULL, &bold, &result);
  printf("li: -> %s\n", status == GTEXT_MARKUP_E_SELECTOR ? "a syntax error" : "something else");
  gtext_markup_query_result_free(&result);
  gtext_markup_document_free(document);
}
