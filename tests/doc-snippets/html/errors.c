#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A document that is wrong in three ways still has a tree. The mistakes are in the result. */
static void errors(void) {
  const char * text = "<p>one<b>two</p>\n</i><p a=1 a=2>";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.max_errors = 4;                   /* keep the first four, count the rest */

  if (gtext_html_parse(text, strlen(text), &opts, &doc, &result) == GTEXT_HTML_OK) {
    for (size_t i = 0; i < gtext_html_result_error_count(&result); i++) {
      GTEXT_HTML_Error e;
      if (gtext_html_result_error_at(&result, i, &e)) {
        printf("%zu:%zu (byte %zu) %s\n", e.line, e.column, e.byte_offset, e.name);
      }
    }
    printf("and %zu more\n", gtext_html_result_errors_dropped(&result));
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);
}
