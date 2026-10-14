/* SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.  An out-of-tree consumer: it includes one public header
 * family through the installed include path and links through pkg-config (tools/consumer/run.sh). */
#include <ghoti.io/text/html.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char * text = "<!DOCTYPE html><title>Hi</title><p class=a>one<p>two</b>";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result result;
  if (gtext_html_parse(text, strlen(text), NULL, &doc, &result) != GTEXT_HTML_OK) {
    fprintf(stderr, "parse: %s\n", result.message);
    gtext_html_result_free(&result);
    return 1;
  }
  printf("%zu parse errors, read as %s\n", gtext_html_result_error_count(&result), result.encoding);
  char * out = NULL;
  size_t len = 0;
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
  if (gtext_html_serialize_buffer(root, GTEXT_HTML_SERIALIZE_CHILDREN, NULL, &out, &len, NULL) != GTEXT_HTML_OK) {
    gtext_html_result_free(&result);
    gtext_markup_document_free(doc);
    return 1;
  }
  printf("%.*s\n", (int)len, out);
  gtext_html_serialize_buffer_free(doc, NULL, out);
  gtext_html_result_free(&result);
  gtext_markup_document_free(doc);
  return 0;
}
