#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Feed a document as it arrives. The tree is the same however the bytes are cut. */
static void incremental(void) {
  const char * pieces[] = {"<!DOCTYPE html><ti", "tle>Caf\xc3", "\xa9</title><p>a&am", "p;b"};
  GTEXT_HTML_Parser * parser = NULL;
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  if (gtext_html_parser_new(NULL, &parser) != GTEXT_HTML_OK) {
    return;
  }
  for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
    GTEXT_HTML_Status status = gtext_html_parser_feed(parser, pieces[i], strlen(pieces[i]));
    if (status != GTEXT_HTML_OK) {
      printf("stopped: %s\n", gtext_html_status_string(status));
      break;                              /* every later call says the same */
    }
  }
  if (gtext_html_parser_finish(parser, &doc, &result) == GTEXT_HTML_OK) {
    printf("%zu nodes, %zu bytes\n", result.node_count, result.input_bytes);
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);
  gtext_html_parser_free(parser);        /* after the document, or before: it makes no difference */
}
