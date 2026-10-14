/* SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.  An out-of-tree consumer: it includes one public header
 * family through the installed include path and links through pkg-config (tools/consumer/run.sh). */
#include <ghoti.io/text/xml.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char * text = "<?xml version=\"1.0\"?><a xmlns:p=\"urn:p\"><p:b k=\"v\">t&amp;u</p:b><![CDATA[<c>]]><!--n--></a>";
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_XML_Error err;
  if (gtext_xml_parse(text, strlen(text), NULL, &doc, &err) != GTEXT_XML_OK) {
    fprintf(stderr, "parse: %s\n", err.message);
    return 1;
  }
  char * out = NULL;
  size_t len = 0;
  GTEXT_XML_Write_Result wr;
  if (gtext_xml_write_buffer(doc, NULL, &out, &len, &wr) != GTEXT_XML_OK) {
    fprintf(stderr, "write: %s\n", wr.message);
    gtext_markup_document_free(doc);
    return 1;
  }
  printf("%zu nodes, %zu bytes written: %.*s\n", gtext_markup_document_node_count(doc), len, (int)len, out);
  gtext_xml_write_buffer_free(doc, NULL, out);
  gtext_markup_document_free(doc);
  return 0;
}
