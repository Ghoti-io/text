#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* A Content-Type header says ISO-8859-2 and the document says windows-1251. A certain label wins and the declaration is not read;
   a tentative one (a guess of the caller's) yields to the declaration. An unusable label is ignored and the result says so. */
static void encoding_transport(void) {
  const char * bytes = "<meta charset=windows-1251><p>x";
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
  GTEXT_HTML_Sniff_Result r;

  opts.transport_encoding = "iso-8859-2";
  gtext_html_sniff_encoding(bytes, strlen(bytes), &opts, &r);
  printf("certain label: %s\n", r.encoding_name);

  opts.transport_encoding_certainty = GTEXT_HTML_TENTATIVE;
  gtext_html_sniff_encoding(bytes, strlen(bytes), &opts, &r);
  printf("tentative label: %s\n", r.encoding_name);

  opts.transport_encoding = "no-such-encoding";
  opts.transport_encoding_certainty = GTEXT_HTML_CERTAIN;
  gtext_html_sniff_encoding(bytes, strlen(bytes), &opts, &r);
  printf("unusable label: %s, label ignored: %s\n", r.encoding_name, r.transport_encoding_ignored ? "yes" : "no");
}
