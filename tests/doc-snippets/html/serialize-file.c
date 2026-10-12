#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A file is replaced atomically: a write that fails leaves the previous one as it was. The limits are statuses of their own. */
static void serialize_file(void) {
  const char * path = "doc-example-out.html";
  const char * text = "<!DOCTYPE html><title>t</title><div><div><div>deep</div></div></div>";
  GTEXT_Markup_Document * doc = NULL;
  if (gtext_html_parse(text, strlen(text), NULL, &doc, NULL) != GTEXT_HTML_OK) {
    return;
  }
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);

  GTEXT_HTML_Serialize_Result result;
  GTEXT_HTML_Status status = gtext_html_serialize_file(root, GTEXT_HTML_SERIALIZE_CHILDREN, path, NULL, &result);
  printf("%s: %zu bytes\n", gtext_html_status_string(status), result.bytes_written);

  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.max_depth = 4;                        /* html, body and two more; 0 is the default, 1024, never "none" */
  status = gtext_html_serialize_file(root, GTEXT_HTML_SERIALIZE_CHILDREN, path, &opts, &result);
  printf("%s\n", gtext_html_status_string(status));

  FILE * in = fopen(path, "rb");             /* the first file is still there */
  if (in) {
    char line[200];
    printf("%s\n", fgets(line, sizeof(line), in) ? line : "");
    fclose(in);
  }
  remove(path);
  gtext_markup_document_free(doc);
}
