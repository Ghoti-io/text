#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Write a tree in Shift_JIS. A character the encoding has is written as its bytes; one it has not is written as a numeric
   character reference where the standard allows one (text and attribute values), so the output reads back as the same text. */
static void encoding_serialize(void) {
  const char * text = "<p title=\"\xe2\x82\xac\">\xe6\x97\xa5\xe6\x9c\xac \xe2\x82\xac</p>";   /* "日本 €" twice */
  GTEXT_HTML_Fragment_Context context;
  memset(&context, 0, sizeof(context));
  context.local_name = "div";

  GTEXT_Markup_Document * doc = NULL;
  GTEXT_Markup_Node * fragment = NULL;
  if (gtext_html_parse_fragment(&context, text, strlen(text), NULL, &doc, &fragment, NULL) != GTEXT_HTML_OK) {
    return;
  }
  GTEXT_HTML_Serialize_Options opts = gtext_html_serialize_options_default();
  opts.encoding = "shift_jis";

  char * out = NULL;
  size_t length = 0;
  GTEXT_HTML_Serialize_Result result;
  if (gtext_html_serialize_buffer(fragment, GTEXT_HTML_SERIALIZE_CHILDREN, &opts, &out, &length, &result) == GTEXT_HTML_OK) {
    printf("%s, %zu bytes, %zu references: ", result.encoding, length, result.references_written);
    for (size_t i = 0; i < length; i++) {   /* (the bytes that are not ASCII, in hexadecimal) */
      if ((unsigned char)out[i] < 0x80) {
        putchar(out[i]);
      }
      else {
        printf("\\x%02x", (unsigned char)out[i]);
      }
    }
    printf("\n");
    gtext_html_serialize_buffer_free(doc, NULL, out);
  }
  gtext_markup_document_free(doc);
}
