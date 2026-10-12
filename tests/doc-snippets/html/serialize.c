#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Parse a fragment and write it back. The output is the standard's serialization: every attribute double quoted,
   `&`, `<`, `>` and the no-break space escaped, no end tag for a void element, and nothing left out. */
static void serialize_fragment(void) {
  const char * text = "<p class=a title='x\"y'>one &amp; two<br><b>three</p>";
  GTEXT_HTML_Fragment_Context context;
  memset(&context, 0, sizeof(context));
  context.local_name = "div";

  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * fragment = NULL;
  if (gtext_html_parse_fragment(&context, text, strlen(text), NULL, &doc, &fragment, NULL) != GTEXT_HTML_OK) {
    return;
  }

  char * out = NULL;
  size_t length = 0;
  GTEXT_HTML_Serialize_Result result;
  if (gtext_html_serialize_buffer(fragment, GTEXT_HTML_SERIALIZE_CHILDREN, NULL, &out, &length, &result)
      == GTEXT_HTML_OK) {
    printf("%s\n%zu bytes, %zu nodes\n", out, result.bytes_written, result.nodes_visited);
    gtext_html_serialize_buffer_free(doc, NULL, out);
  }

  /* One element, itself: `outerHTML`. */
  const GTEXT_Markup_Node * p = gtext_markup_node_first_child(fragment);
  if (gtext_html_serialize_buffer(p, GTEXT_HTML_SERIALIZE_OUTER, NULL, &out, &length, NULL) == GTEXT_HTML_OK) {
    printf("%s\n", out);
    gtext_html_serialize_buffer_free(doc, NULL, out);
  }
  gtext_markup_document_free(doc);
}
