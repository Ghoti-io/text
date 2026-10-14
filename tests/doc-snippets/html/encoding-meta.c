#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* Bytes in an encoding the document declares. The title is "Привет" in windows-1251. The first document declares it in the first 1024
   bytes, which the parser reads before it begins; in the second the declaration comes after a long comment, so the parser meets it as an
   element, throws what it has built away and begins again in that encoding. The tree is the same. */
static void encoding_meta(void) {
  static char late[1400];
  snprintf(late, sizeof(late), "<!doctype html><!--%*s--><meta charset=windows-1251><title>\xcf\xf0\xe8\xe2\xe5\xf2</title>", 1100, "");
  const char * documents[] = {
    "<!doctype html><meta charset=windows-1251><title>\xcf\xf0\xe8\xe2\xe5\xf2</title>",
    late,
  };
  for (size_t i = 0; i < 2; i++) {
    GTEXT_Markup_Document * doc = NULL;
    GTEXT_HTML_Result result;
    if (gtext_html_parse(documents[i], strlen(documents[i]), NULL, &doc, &result) == GTEXT_HTML_OK) {
      printf("%s, parsed again: %s\n", result.encoding, result.reparsed ? "yes" : "no");
      gtext_markup_document_free(doc);
    }
    gtext_html_result_free(&result);
  }
}
