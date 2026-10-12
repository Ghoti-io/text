#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* The scripting flag decides how <noscript> is read. Nothing is ever executed. */
static void scripting(void) {
  const char * text = "<body><noscript><p>enable scripts</p></noscript>";
  GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();

  for (int flag = 0; flag < 2; flag++) {
    opts.scripting = flag != 0;
    GTEXT_Markup_Document * doc = NULL;
    if (gtext_html_parse(text, strlen(text), &opts, &doc, NULL) != GTEXT_HTML_OK) {
      continue;
    }
    const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
    for (const GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n;
         n = gtext_markup_node_next_in_order(root, n, false)) {
      if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
          && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "noscript")) {
        const GTEXT_Markup_Node * child = gtext_markup_node_first_child(n);
        if (gtext_markup_node_kind(child) == GTEXT_MARKUP_KIND_TEXT) {
          printf("scripting %d: the noscript holds the text \"%s\"\n", flag, gtext_markup_node_data(child, NULL));
        }
        else {
          printf("scripting %d: the noscript holds a <%s> element\n", flag, gtext_markup_element_local_name(child));
        }
      }
    }
    gtext_markup_document_free(doc);
  }
}
