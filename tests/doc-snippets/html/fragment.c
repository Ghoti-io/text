#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

/* What `td.innerHTML = "<p>x</p>y"` builds, and moving it into another document. */
static void fragment(void) {
  GTEXT_HTML_Fragment_Context context;
  memset(&context, 0, sizeof(context));
  context.ns = GTEXT_HTML_NS_HTML;
  context.local_name = "td";

  const char * text = "<p>x</p>y";
  GTEXT_Markup_Document * scratch = NULL;     /* owns the fragment */
  GTEXT_Markup_Node * fragment = NULL;
  if (gtext_html_parse_fragment(&context, text, strlen(text), NULL, &scratch, &fragment, NULL)
      != GTEXT_HTML_OK) {
    return;
  }
  printf("%zu children\n", gtext_markup_node_child_count(fragment));

  /* A page of your own to put it in. */
  const char * html = "<table><tr><td id=t>";
  GTEXT_Markup_Document * page = NULL;
  if (gtext_html_parse(html, strlen(html), NULL, &page, NULL) == GTEXT_HTML_OK) {
    const GTEXT_Markup_Node * root = gtext_markup_document_root(page);
    GTEXT_Markup_Node * cell = NULL;
    for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n && !cell;
         n = gtext_markup_node_next_in_order(root, n, false)) {
      if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT
          && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "td")) {
        cell = n;
      }
    }
    GTEXT_Markup_Node * copy = NULL;
    /* Clone the fragment into the page, then insert the copy: a fragment inserts its children. */
    if (cell && gtext_markup_clone(page, fragment, true, &copy) == GTEXT_MARKUP_OK
        && gtext_markup_append_child(cell, copy) == GTEXT_MARKUP_OK) {
      printf("the cell now has %zu children\n", gtext_markup_node_child_count(cell));
    }
    gtext_markup_document_free(page);
  }
  gtext_markup_document_free(scratch);       /* and the fragment is gone with it */
}
