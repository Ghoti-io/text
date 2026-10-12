#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Parse a document, say how it was read, and print its title. */
static void quick_start(void) {
  const char * text = "<!DOCTYPE html><title>Hi</title><p class=a>one<p>two</b>";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  if (gtext_html_parse(text, strlen(text), NULL, &doc, &result) != GTEXT_HTML_OK) {
    fprintf(stderr, "not parsed: %s\n", result.message);
    gtext_html_result_free(&result);
    return;
  }

  const char * mode = result.mode == GTEXT_MARKUP_MODE_NO_QUIRKS ? "no-quirks"
      : result.mode == GTEXT_MARKUP_MODE_LIMITED_QUIRKS ? "limited-quirks" : "quirks";
  printf("%s mode, %zu parse errors, read as %s\n", mode,
      gtext_html_result_error_count(&result), result.encoding);

  /* The title is the text of the first element of that name. */
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
  for (const GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n;
       n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
        && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "title")) {
      char * title = NULL;
      if (gtext_markup_node_text_content(n, NULL, &title, NULL) == GTEXT_MARKUP_OK) {
        printf("title: %s\n", title);
        gtext_markup_string_free(NULL, title);
      }
      break;
    }
  }

  gtext_html_result_free(&result);   /* the result and the document are separate */
  gtext_markup_document_free(doc);
}
