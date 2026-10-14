#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

static const char * how(GTEXT_Markup_Encoding_Source source) {
  switch (source) {
    case GTEXT_MARKUP_ENCODING_BOM: return "from a byte order mark";
    case GTEXT_MARKUP_ENCODING_CALLER: return "from the caller's label";
    case GTEXT_MARKUP_ENCODING_DECLARATION: return "from a declaration";
    case GTEXT_MARKUP_ENCODING_CALLER_DEFAULT: return "from the caller's default";
    case GTEXT_MARKUP_ENCODING_DEFAULT: return "by default";
    default: return "unknown";
  }
}

/* Which encoding would these bytes be read as, and why? Nothing is parsed: this is the decision the parser starts with. */
static void encoding_sniff(void) {
  const char * documents[] = {
    "\xef\xbb\xbf<p>x",                                   /* a byte order mark */
    "<!doctype html><meta charset=windows-1251><p>x",     /* a declaration in the first 1024 bytes */
    "<p>x",                                               /* nothing says */
  };
  for (size_t i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
    GTEXT_HTML_Sniff_Result r;
    if (gtext_html_sniff_encoding(documents[i], strlen(documents[i]), NULL, &r) == GTEXT_HTML_OK) {
      printf("%s, %s, %s\n", r.encoding_name, how(r.source), r.confidence == GTEXT_HTML_CERTAIN ? "certain" : "tentative");
    }
  }
}
