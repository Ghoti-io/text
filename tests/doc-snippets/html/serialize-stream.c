#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

static GTEXT_Markup_Status to_stdout(void * user, const char * bytes, size_t length) {
  (void)user;
  return fwrite(bytes, 1, length, stdout) == length ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_WRITE;
}

/* Write HTML that is not a markup tree: the token stream the tree walk is made of. The serializer holds back at most one
   tag, so `</li>` can be left out when `<li>` follows. */
static void serialize_stream(void) {
  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.omit_optional_tags = true;
  GTEXT_Markup_Sink sink = {to_stdout, NULL};
  GTEXT_HTML_Serializer * ser = NULL;
  if (gtext_html_serializer_new(&opts, &sink, &ser) != GTEXT_HTML_OK) {
    return;
  }

  GTEXT_Markup_Attribute id;
  memset(&id, 0, sizeof(id));
  id.local = "id";
  id.value = "list";
  id.value_length = 4;

  gtext_html_serializer_start_tag(ser, NULL, "ul", &id, 1);
  for (int i = 1; i <= 3; i++) {
    char item[32];
    snprintf(item, sizeof(item), "item <%d>", i);
    gtext_html_serializer_start_tag(ser, NULL, "li", NULL, 0);
    gtext_html_serializer_characters(ser, item, strlen(item));
    gtext_html_serializer_end_tag(ser, NULL, "li");
  }
  gtext_html_serializer_end_tag(ser, NULL, "ul");

  GTEXT_HTML_Serialize_Result result;
  gtext_html_serializer_finish(ser, &result);
  printf("\n%zu bytes, %zu end tags left out\n", result.bytes_written, result.omitted_end_tags);
  gtext_html_serializer_free(ser);
}
