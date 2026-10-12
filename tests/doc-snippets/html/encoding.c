#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The encoding the bytes were read as, and how that was decided. */
static void encoding(void) {
  const char * with_mark = "\xef\xbb\xbf<p>x";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;

  if (gtext_html_parse(with_mark, strlen(with_mark), NULL, &doc, &result) == GTEXT_HTML_OK) {
    printf("%s, from a byte order mark: %d\n", result.encoding,
        result.encoding_source == GTEXT_MARKUP_ENCODING_BOM);
    gtext_markup_document_free(doc);
  }
  gtext_html_result_free(&result);

  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.transport_encoding = "windows-1252";   /* a Content-Type header said so */
  GTEXT_HTML_Status status = gtext_html_parse("<p>x", 4, &opts, &doc, &result);
  printf("%s\n", gtext_html_status_string(status));
  gtext_html_result_free(&result);
}
