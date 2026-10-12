#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Limits keep untrusted input in bounds. Hitting one is a failure with a status of its own. */
static void limits(void) {
  const char * text = "<div><div><div><div><div>deep";
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  opts.max_open_elements = 4;            /* html, body and two more */
  GTEXT_HTML_Status status = gtext_html_parse(text, strlen(text), &opts, &doc, &result);
  if (status == GTEXT_HTML_E_LIMIT_DEPTH) {
    printf("refused: %s\n", result.message);   /* and there is no document to free */
  }
  gtext_html_result_free(&result);

  opts.max_open_elements = 0;            /* 0 is the default, 512; it is never "no limit" */
  status = gtext_html_parse(text, strlen(text), &opts, &doc, &result);
  if (status == GTEXT_HTML_OK) {
    printf("accepted under the default limit\n");
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);
}
