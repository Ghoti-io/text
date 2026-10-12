#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The options are a superset of the standard's, off unless asked for. Here the tags HTML lets a document leave out are
   left out, and an attribute is quoted only where the syntax needs it. */
static void serialize_options(void) {
  const char * text = "<!DOCTYPE html><html><head><title>Hello</title></head><body>"
                      "<ul><li class=a>one<li>two</ul><p title=\"a b\">Welcome</p></body></html>";
  GTEXT_Markup_Document * doc = NULL;
  if (gtext_html_parse(text, strlen(text), NULL, &doc, NULL) != GTEXT_HTML_OK) {
    return;
  }

  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.omit_optional_tags = true;
  opts.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;

  char * out = NULL;
  size_t length = 0;
  GTEXT_HTML_Serialize_Result result;
  if (gtext_html_serialize_buffer(gtext_markup_document_root(doc), GTEXT_HTML_SERIALIZE_CHILDREN, &opts, &out,
          &length, &result) == GTEXT_HTML_OK) {
    printf("%s\n%zu start tags and %zu end tags left out\n", out, result.omitted_start_tags,
        result.omitted_end_tags);
    gtext_html_serialize_buffer_free(doc, &opts, out);
  }

  /* A combination that means nothing is refused before anything is written. */
  opts.space_before_trailing_solidus = true;
  GTEXT_HTML_Status status = gtext_html_serialize_buffer(gtext_markup_document_root(doc),
      GTEXT_HTML_SERIALIZE_CHILDREN, &opts, &out, &length, NULL);
  printf("%s\n", gtext_html_status_string(status));
  gtext_markup_document_free(doc);
}
