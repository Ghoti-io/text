/* SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.  An out-of-tree consumer: it includes one public header
 * family through the installed include path and links through pkg-config (tools/consumer/run.sh). */
#include <ghoti.io/text/markup.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  GTEXT_Markup_Document * doc = NULL;
  if (gtext_markup_document_new(NULL, &doc) != GTEXT_MARKUP_OK) {
    return 1;
  }
  GTEXT_Markup_Node * el = NULL;
  GTEXT_Markup_Node * text = NULL;
  if (gtext_markup_create_element(doc, GTEXT_MARKUP_NS_HTML, NULL, "p", &el) != GTEXT_MARKUP_OK
      || gtext_markup_element_set_attribute(el, NULL, NULL, "class", "a<b", 3) != GTEXT_MARKUP_OK
      || gtext_markup_create_text(doc, "x & y", 5, &text) != GTEXT_MARKUP_OK
      || gtext_markup_append_child(el, text) != GTEXT_MARKUP_OK
      || gtext_markup_append_child(gtext_markup_document_root(doc), el) != GTEXT_MARKUP_OK) {
    gtext_markup_document_free(doc);
    return 1;
  }
  gtext_markup_dump(gtext_markup_document_root(doc), stdout);
  printf("%zu nodes\n", gtext_markup_document_node_count(doc));
  gtext_markup_document_free(doc);
  return 0;
}
