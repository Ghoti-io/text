#include <ghoti.io/text/html.h>
#include <stdio.h>

/* Parse a file. A path that cannot be read is an ordinary failure with a message. */
static void file(void) {
  const char * path = "doc-example.html";
  FILE * out = fopen(path, "wb");          /* something to read */
  if (!out) {
    return;
  }
  fputs("<!DOCTYPE html><title>t</title><p>hello\n", out);
  fclose(out);

  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  opts.max_input_bytes = 1 << 20;         /* refuse a file over a megabyte while it is read */

  GTEXT_HTML_Status status = gtext_html_parse_file(path, &opts, &doc, &result);
  if (status == GTEXT_HTML_OK) {
    printf("read %zu bytes\n", result.input_bytes);
    gtext_markup_document_free(doc);
  }
  else {
    printf("%s: %s\n", gtext_html_status_string(status), result.message);
  }
  gtext_html_result_free(&result);

  remove(path);
  status = gtext_html_parse_file(path, NULL, &doc, &result);   /* gone now */
  printf("%s: %s\n", gtext_html_status_string(status), result.message);
  gtext_html_result_free(&result);
}
